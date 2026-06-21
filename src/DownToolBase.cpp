#include "DownToolBase.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "cpl_json.h"
#include "fmt/format.h"
#include "libxml/tree.h"
#include "libxml/xmlmemory.h"
#include "opencv2/core/utility.hpp"
#include "opencv2/imgcodecs.hpp"
#include "opencv2/imgproc.hpp"

namespace
{
	enum class ImageType { Unknown, Png, Jpeg };

	enum class TileRepairWriteResult { Failed, Existing, Written };

	constexpr uint64_t MaxBatchRepairedTileCount = 4096;
	constexpr size_t CacheScanBatchSize = 128;
	constexpr size_t MaxCacheIssueSampleCount = 20;
	constexpr uint32_t CacheScanConcurrencyMultiplier = 4;

	struct TileRepairTask
	{
		DownTool::Tile childTile;
		uint32_t childX = 0;
		uint32_t childY = 0;
	};

	struct TileSample
	{
		DownTool::Tile sourceTile;
		double pixelX = 0.;
		double pixelY = 0.;
		bool valid = false;
	};

	struct PostProcessBounds
	{
		double west = 0.;
		double east = 0.;
		double south = 0.;
		double north = 0.;
	};

	struct PostProcessMetadata
	{
		PostProcessBounds bounds;
		uint32_t maxZoom = 0;
		uint32_t tileSize = 256;
	};

	struct MergedImageOutputSize
	{
		uint32_t width = 0;
		uint32_t height = 0;
	};

	bool IsSameTile(const DownTool::Tile& left, const DownTool::Tile& right)
	{
		return left.X == right.X && left.Y == right.Y && left.Zoom == right.Zoom;
	}

	uint32_t GetEffectiveQueueCount(uint32_t configuredCount)
	{
		const uint32_t opencvThreadCount =
			static_cast<uint32_t>(std::max(1, cv::getNumThreads()));
		if (configuredCount == 0)
		{
			return opencvThreadCount;
		}

		return std::max(1u, configuredCount);
	}

	uint32_t GetHardwareThreadCount()
	{
		return std::max(1u, std::thread::hardware_concurrency());
	}

	uint32_t GetParallelWorkerCount(uint64_t itemCount, uint32_t configuredQueueCount)
	{
		if (itemCount == 0)
		{
			return 0;
		}

		const uint32_t hardwareThreadCount = GetHardwareThreadCount();
		const uint32_t queueCount = configuredQueueCount == 0 ?
			hardwareThreadCount :
			std::min(configuredQueueCount, hardwareThreadCount);
		return static_cast<uint32_t>(
			std::min<uint64_t>(itemCount, std::max(1u, queueCount)));
	}

	template<typename Function>
	void ParallelForIndex(size_t itemCount, uint32_t configuredQueueCount, Function function)
	{
		const uint32_t workerCount = GetParallelWorkerCount(itemCount, configuredQueueCount);
		if (workerCount <= 1)
		{
			for (size_t index = 0; index < itemCount; index++)
			{
				function(index);
			}
			return;
		}

		std::atomic_size_t nextIndex = 0;
		std::vector<std::thread> workers;
		workers.reserve(workerCount);
		for (uint32_t workerIndex = 0; workerIndex < workerCount; workerIndex++)
		{
			(void)workerIndex;
			workers.emplace_back(
				[&]()
				{
					for (;;)
					{
						const size_t index =
							nextIndex.fetch_add(1, std::memory_order_relaxed);
						if (index >= itemCount)
						{
							break;
						}
						function(index);
					}
				});
		}

		for (std::thread& worker : workers)
		{
			if (worker.joinable())
			{
				worker.join();
			}
		}
	}

	double SecondsSince(const std::chrono::steady_clock::time_point& startTime)
	{
		return std::chrono::duration<double>(
			std::chrono::steady_clock::now() - startTime).count();
	}

	uint32_t GetCacheScanWorkerCount(uint64_t tileCount)
	{
		if (tileCount == 0)
		{
			return 0;
		}

		const uint64_t workerLimit =
			static_cast<uint64_t>(GetHardwareThreadCount()) * CacheScanConcurrencyMultiplier;
		const uint64_t workerCount = std::min<uint64_t>(tileCount, workerLimit);
		return static_cast<uint32_t>(std::max<uint64_t>(1, workerCount));
	}

	const char* GetTileCacheStateText(DownTool::TileCacheState state)
	{
		switch (state)
		{
		case DownTool::TileCacheState::Ready:
			return "可用";
		case DownTool::TileCacheState::Missing:
			return "缺失";
		case DownTool::TileCacheState::Unavailable:
			return "占位图";
		case DownTool::TileCacheState::Invalid:
			return "无效文件";
		default:
			return "未知";
		}
	}

	DownTool::Tile GetFirstChildTile(
		const DownTool::Tile& parentTile, uint32_t targetZoom)
	{
		if (targetZoom <= parentTile.Zoom)
		{
			return parentTile;
		}

		const uint32_t zoomOffset = targetZoom - parentTile.Zoom;
		if (zoomOffset >= 31)
		{
			return parentTile;
		}

		const uint64_t scale = uint64_t{ 1 } << zoomOffset;
		return DownTool::Tile(
			static_cast<uint32_t>(static_cast<uint64_t>(parentTile.X) * scale),
			static_cast<uint32_t>(static_cast<uint64_t>(parentTile.Y) * scale),
			targetZoom);
	}

	std::string TrimAsciiWhitespace(const std::string& text)
	{
		size_t begin = 0;
		while (begin < text.size() &&
			std::isspace(static_cast<unsigned char>(text[begin])) != 0)
		{
			begin++;
		}

		size_t end = text.size();
		while (end > begin &&
			std::isspace(static_cast<unsigned char>(text[end - 1])) != 0)
		{
			end--;
		}

		return text.substr(begin, end - begin);
	}

	std::string ToLowerAscii(std::string text)
	{
		for (char& character : text)
		{
			character = static_cast<char>(
				std::tolower(static_cast<unsigned char>(character)));
		}

		return text;
	}

	size_t HashCombine(size_t seed, size_t value) noexcept
	{
		return seed ^ (value + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2));
	}

	struct TileHash
	{
		size_t operator()(const DownTool::Tile& tile) const noexcept
		{
			size_t seed = std::hash<uint32_t>{}(tile.Zoom);
			seed = HashCombine(seed, std::hash<uint32_t>{}(tile.X));
			seed = HashCombine(seed, std::hash<uint32_t>{}(tile.Y));
			return seed;
		}
	};

	struct TileEqual
	{
		bool operator()(const DownTool::Tile& left, const DownTool::Tile& right) const noexcept
		{
			return IsSameTile(left, right);
		}
	};

	ImageType DetectImageType(const unsigned char* data, size_t len)
	{
		if (len >= 8 &&
			data[0] == 0x89 && data[1] == 0x50 && data[2] == 0x4E && data[3] == 0x47 &&
			data[4] == 0x0D && data[5] == 0x0A && data[6] == 0x1A && data[7] == 0x0A)
			return ImageType::Png;

		if (len >= 3 &&
			data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF)
			return ImageType::Jpeg;

		return ImageType::Unknown;
	}

	size_t WriteTileData(void* ptr, size_t size, size_t nmemb, void* userdata)
	{
		DownTool::CurlMultiHandle* context = static_cast<DownTool::CurlMultiHandle*>(userdata);
		size_t totalSize = size * nmemb;
		if (context->fileStream.is_open())
		{
			context->fileStream.write(static_cast<char*>(ptr), totalSize);
		}
		return totalSize;
	}

	size_t WriteTileHeaderData(void* ptr, size_t size, size_t nmemb, void* userdata)
	{
		DownTool::CurlMultiHandle* context = static_cast<DownTool::CurlMultiHandle*>(userdata);
		const size_t totalSize = size * nmemb;
		const std::string headerLine(static_cast<char*>(ptr), totalSize);
		const size_t separatorIndex = headerLine.find(':');
		if (separatorIndex == std::string::npos)
		{
			return totalSize;
		}

		std::string headerName = ToLowerAscii(TrimAsciiWhitespace(
			headerLine.substr(0, separatorIndex)));
		std::string headerValue = TrimAsciiWhitespace(
			headerLine.substr(separatorIndex + 1));
		if (!headerName.empty())
		{
			context->responseHeaders[std::move(headerName)] = std::move(headerValue);
		}

		return totalSize;
	}

	bool IsValidImageFile(const std::filesystem::path& path)
	{
		std::ifstream fileStream(path, std::ios::binary);
		if (!fileStream.is_open())
		{
			return false;
		}

		unsigned char header[8];
		fileStream.read(reinterpret_cast<char*>(header), sizeof(header));
		return DetectImageType(header, fileStream.gcount()) != ImageType::Unknown;
	}

	cv::Mat ReadImage(const std::filesystem::path& path)
	{
		std::ifstream fileStream(path, std::ios::binary);
		if (!fileStream.is_open())
		{
			return {};
		}

		std::vector<unsigned char> data(
			(std::istreambuf_iterator<char>(fileStream)), std::istreambuf_iterator<char>());
		if (data.empty())
		{
			return {};
		}

		try
		{
			return cv::imdecode(data, cv::IMREAD_COLOR);
		}
		catch (const cv::Exception&)
		{
			return {};
		}
	}

	cv::Mat ReadImageUnchanged(const std::filesystem::path& path)
	{
		std::ifstream fileStream(path, std::ios::binary);
		if (!fileStream.is_open())
		{
			return {};
		}

		std::vector<unsigned char> data(
			(std::istreambuf_iterator<char>(fileStream)), std::istreambuf_iterator<char>());
		if (data.empty())
		{
			return {};
		}

		try
		{
			return cv::imdecode(data, cv::IMREAD_UNCHANGED);
		}
		catch (const cv::Exception&)
		{
			return {};
		}
	}

	cv::Mat EnsureBgra(const cv::Mat& image)
	{
		if (image.empty())
		{
			return {};
		}

		if (image.channels() == 4)
		{
			return image;
		}

		cv::Mat bgraImage;
		if (image.channels() == 3)
		{
			cv::cvtColor(image, bgraImage, cv::COLOR_BGR2BGRA);
			return bgraImage;
		}
		if (image.channels() == 1)
		{
			cv::cvtColor(image, bgraImage, cv::COLOR_GRAY2BGRA);
			return bgraImage;
		}

		return {};
	}

	cv::Mat ReadBgraTile(const std::filesystem::path& path, uint32_t tileSize)
	{
		cv::Mat image = EnsureBgra(ReadImageUnchanged(path));
		if (image.empty())
		{
			return {};
		}

		if (image.cols == static_cast<int>(tileSize) &&
			image.rows == static_cast<int>(tileSize))
		{
			return image;
		}

		cv::Mat resizedImage;
		cv::resize(
			image,
			resizedImage,
			cv::Size(static_cast<int>(tileSize), static_cast<int>(tileSize)),
			0.,
			0.,
			cv::INTER_LINEAR);
		return resizedImage;
	}

	cv::Mat ResizeImageToSize(const cv::Mat& image, int width, int height)
	{
		if (image.empty() || width <= 0 || height <= 0)
		{
			return {};
		}

		if (image.cols == width && image.rows == height)
		{
			return image;
		}

		cv::Mat resizedImage;
		const cv::InterpolationFlags interpolation =
			(image.cols > width || image.rows > height) ?
			cv::INTER_AREA : cv::INTER_LINEAR;
		cv::resize(
			image,
			resizedImage,
			cv::Size(width, height),
			0.,
			0.,
			interpolation);
		return resizedImage;
	}

	cv::Vec4b SampleBilinear(const cv::Mat& image, double pixelX, double pixelY)
	{
		const double clampedX =
			std::clamp(pixelX, 0., static_cast<double>(image.cols - 1));
		const double clampedY =
			std::clamp(pixelY, 0., static_cast<double>(image.rows - 1));
		const int x0 = static_cast<int>(std::floor(clampedX));
		const int y0 = static_cast<int>(std::floor(clampedY));
		const int x1 = std::min(x0 + 1, image.cols - 1);
		const int y1 = std::min(y0 + 1, image.rows - 1);
		const double weightX = clampedX - x0;
		const double weightY = clampedY - y0;

		const cv::Vec4b c00 = image.at<cv::Vec4b>(y0, x0);
		const cv::Vec4b c10 = image.at<cv::Vec4b>(y0, x1);
		const cv::Vec4b c01 = image.at<cv::Vec4b>(y1, x0);
		const cv::Vec4b c11 = image.at<cv::Vec4b>(y1, x1);

		cv::Vec4b result;
		for (int channel = 0; channel < 4; channel++)
		{
			const double top =
				static_cast<double>(c00[channel]) * (1. - weightX) +
				static_cast<double>(c10[channel]) * weightX;
			const double bottom =
				static_cast<double>(c01[channel]) * (1. - weightX) +
				static_cast<double>(c11[channel]) * weightX;
			result[channel] = static_cast<unsigned char>(
				std::clamp(top * (1. - weightY) + bottom * weightY, 0., 255.));
		}

		return result;
	}

	bool CopyFileReplacing(
		const std::filesystem::path& sourcePath,
		const std::filesystem::path& targetPath)
	{
		std::error_code createDirectoryError;
		std::filesystem::create_directories(targetPath.parent_path(), createDirectoryError);
		if (createDirectoryError)
		{
			return false;
		}

		std::error_code copyError;
		std::filesystem::copy_file(
			sourcePath,
			targetPath,
			std::filesystem::copy_options::overwrite_existing,
			copyError);
		return !copyError;
	}

	bool RemoveFileIfExists(const std::filesystem::path& path)
	{
		std::error_code removeError;
		std::filesystem::remove(path, removeError);
		return !removeError;
	}

	bool WriteTextFile(const std::filesystem::path& path, const std::string& content)
	{
		std::error_code createDirectoryError;
		std::filesystem::create_directories(path.parent_path(), createDirectoryError);
		if (createDirectoryError)
		{
			return false;
		}

		std::ofstream fileStream(path, std::ios::binary);
		if (!fileStream.is_open())
		{
			return false;
		}

		fileStream.write(content.data(), static_cast<std::streamsize>(content.size()));
		return fileStream.good();
	}

	bool ReadTextFile(const std::filesystem::path& path, std::string& content)
	{
		std::ifstream fileStream(path, std::ios::binary);
		if (!fileStream.is_open())
		{
			return false;
		}

		content.assign(
			std::istreambuf_iterator<char>(fileStream),
			std::istreambuf_iterator<char>());
		return fileStream.good() || fileStream.eof();
	}

	std::string PathToUtf8String(const std::filesystem::path& path)
	{
		const std::u8string utf8Path = path.u8string();
		std::string result;
		result.reserve(utf8Path.size());
		for (char8_t character : utf8Path)
		{
			result.push_back(static_cast<char>(character));
		}

		return result;
	}

	std::filesystem::path MakePathWithUtf8Suffix(
		const std::filesystem::path& path,
		const std::string& suffix)
	{
		return std::filesystem::u8path(PathToUtf8String(path) + suffix);
	}

	double LongitudeToWebMercatorX(double longitude)
	{
		constexpr double earthRadius = 6378137.;
		constexpr double pi = 3.141592653589793238462643383279502884;
		return earthRadius * longitude * pi / 180.;
	}

	double LatitudeToWebMercatorY(double latitude)
	{
		constexpr double earthRadius = 6378137.;
		constexpr double pi = 3.141592653589793238462643383279502884;
		constexpr double maxMercatorLatitude = 85.05112877980659;
		const double clampedLatitude =
			std::clamp(latitude, -maxMercatorLatitude, maxMercatorLatitude);
		const double latitudeRadians = clampedLatitude * pi / 180.;
		return earthRadius * std::log(std::tan(pi / 4. + latitudeRadians / 2.));
	}

	const xmlChar* ToXmlChar(const char* text)
	{
		return reinterpret_cast<const xmlChar*>(text);
	}

	const xmlChar* ToXmlChar(const std::string& text)
	{
		return reinterpret_cast<const xmlChar*>(text.c_str());
	}

	std::string FormatMetadataDouble(double value)
	{
		return fmt::format("{:.15g}", value);
	}

	bool SetXmlProperty(xmlNodePtr node, const char* name, const std::string& value)
	{
		return xmlSetProp(node, ToXmlChar(name), ToXmlChar(value)) != nullptr;
	}

	bool SetXmlProperty(xmlNodePtr node, const char* name, const char* value)
	{
		return xmlSetProp(node, ToXmlChar(name), ToXmlChar(value)) != nullptr;
	}

	xmlNodePtr AddXmlChild(xmlNodePtr parent, const char* name, const char* content)
	{
		return xmlNewChild(parent, nullptr, ToXmlChar(name), ToXmlChar(content));
	}

	xmlNodePtr AddXmlChild(xmlNodePtr parent, const char* name)
	{
		return xmlNewChild(parent, nullptr, ToXmlChar(name), nullptr);
	}

	CPLJSONObject BuildBoundsJson(const PostProcessBounds& bounds)
	{
		CPLJSONObject jsonBounds;
		jsonBounds.Add("east", bounds.east);
		jsonBounds.Add("north", bounds.north);
		jsonBounds.Add("south", bounds.south);
		jsonBounds.Add("west", bounds.west);
		return jsonBounds;
	}

	std::string BuildMetaJson(const PostProcessMetadata& metadata)
	{
		CPLJSONObject root;
		root.Add("bounds", BuildBoundsJson(metadata.bounds));
		root.Add("contentType", "image/png");
		root.Add("generatetool", "DomDownTool");
		root.Add("latLonBounds", BuildBoundsJson(metadata.bounds));
		root.Add("maxzoom", static_cast<int>(metadata.maxZoom));
		root.Add("minzoom", 0);
		root.Add("proj", 4326);
		root.Add("tilesize", static_cast<int>(metadata.tileSize));
		root.Add("tiletrans", "google");
		root.Add("type", "image");
		root.Add("ziped", false);
		return root.Format(CPLJSONObject::PrettyFormat::Plain);
	}

	std::string BuildMergedImageCacheMetadata(
		uint32_t zoom,
		uint32_t sampleZoom,
		uint32_t tileSize,
		uint32_t targetMaxResolution,
		uint32_t outputWidth,
		uint32_t outputHeight,
		uint32_t minTileX,
		uint32_t minTileY,
		uint32_t maxTileX,
		uint32_t maxTileY,
		const std::string& cachePath)
	{
		CPLJSONObject tileRange;
		tileRange.Add("maxx", static_cast<int>(maxTileX));
		tileRange.Add("maxy", static_cast<int>(maxTileY));
		tileRange.Add("minx", static_cast<int>(minTileX));
		tileRange.Add("miny", static_cast<int>(minTileY));

		CPLJSONObject root;
		root.Add("cachePath", cachePath);
		root.Add("outputheight", static_cast<int>(outputHeight));
		root.Add("outputwidth", static_cast<int>(outputWidth));
		root.Add("samplezoom", static_cast<int>(sampleZoom));
		root.Add("targetmaxresolution", static_cast<int>(targetMaxResolution));
		root.Add("tilerange", tileRange);
		root.Add("tilesize", static_cast<int>(tileSize));
		root.Add("version", 2);
		root.Add("zoom", static_cast<int>(zoom));
		return root.Format(CPLJSONObject::PrettyFormat::Plain);
	}

	MergedImageOutputSize CalculateMergedImageOutputSize(
		uint64_t tileXCount,
		uint64_t tileYCount,
		uint32_t tileSize,
		uint32_t targetMaxResolution)
	{
		if (tileXCount == 0 || tileYCount == 0 || tileSize == 0 ||
			targetMaxResolution == 0)
		{
			return {};
		}

		const uint64_t maxUint64 = std::numeric_limits<uint64_t>::max();
		if (tileXCount > maxUint64 / tileSize || tileYCount > maxUint64 / tileSize)
		{
			return {};
		}

		const uint64_t sourceWidth = tileXCount * tileSize;
		const uint64_t sourceHeight = tileYCount * tileSize;
		const uint64_t maxSourceSize = std::max(sourceWidth, sourceHeight);
		if (maxSourceSize == 0)
		{
			return {};
		}

		if (maxSourceSize <= targetMaxResolution)
		{
			return {
				static_cast<uint32_t>(sourceWidth),
				static_cast<uint32_t>(sourceHeight)
			};
		}

		const auto scaleToTarget = [maxSourceSize, targetMaxResolution](
			uint64_t sourceSize)
		{
			const long double scaledSize =
				static_cast<long double>(sourceSize) * targetMaxResolution /
				static_cast<long double>(maxSourceSize);
			const long double roundedSize = std::round(scaledSize);
			const long double clampedSize = std::clamp(
				roundedSize,
				1.0L,
				static_cast<long double>(targetMaxResolution));
			return static_cast<uint32_t>(clampedSize);
		};

		return {
			scaleToTarget(sourceWidth),
			scaleToTarget(sourceHeight)
		};
	}

	bool IsMergedImageCacheReady(
		const std::filesystem::path& imagePath,
		const std::filesystem::path& metadataPath,
		const std::string& expectedMetadata)
	{
		std::error_code existsError;
		if (!std::filesystem::exists(imagePath, existsError) || existsError)
		{
			return false;
		}

		std::error_code fileSizeError;
		if (std::filesystem::file_size(imagePath, fileSizeError) == 0 || fileSizeError)
		{
			return false;
		}

		std::string actualMetadata;
		return ReadTextFile(metadataPath, actualMetadata) &&
			actualMetadata == expectedMetadata;
	}

	std::string BuildTileMapResourceXml(const PostProcessMetadata& metadata)
	{
		using XmlDocPtr = std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)>;
		struct XmlBufferDeleter
		{
			void operator()(xmlChar* buffer) const
			{
				xmlFree(buffer);
			}
		};
		using XmlBufferPtr = std::unique_ptr<xmlChar, XmlBufferDeleter>;

		XmlDocPtr document(xmlNewDoc(ToXmlChar("1.0")), xmlFreeDoc);
		if (!document)
		{
			return {};
		}

		xmlNodePtr root = xmlNewNode(nullptr, ToXmlChar("TileMap"));
		if (!root)
		{
			return {};
		}
		xmlDocSetRootElement(document.get(), root);

		if (!SetXmlProperty(root, "version", "1.0.0") ||
			!SetXmlProperty(root, "tilemapservice", "http://tms.osgeo.org/1.0.0"))
		{
			return {};
		}

		if (!AddXmlChild(root, "Title", "WMTS") ||
			!AddXmlChild(root, "SRS", "EPSG:3857"))
		{
			return {};
		}

		const double westMercator = LongitudeToWebMercatorX(metadata.bounds.west);
		const double eastMercator = LongitudeToWebMercatorX(metadata.bounds.east);
		const double southMercator = LatitudeToWebMercatorY(metadata.bounds.south);
		const double northMercator = LatitudeToWebMercatorY(metadata.bounds.north);

		xmlNodePtr boundingBox = AddXmlChild(root, "BoundingBox");
		if (!boundingBox ||
			!SetXmlProperty(boundingBox, "miny", FormatMetadataDouble(southMercator)) ||
			!SetXmlProperty(boundingBox, "minx", FormatMetadataDouble(westMercator)) ||
			!SetXmlProperty(boundingBox, "maxy", FormatMetadataDouble(northMercator)) ||
			!SetXmlProperty(boundingBox, "maxx", FormatMetadataDouble(eastMercator)))
		{
			return {};
		}

		xmlNodePtr origin = AddXmlChild(root, "Origin");
		if (!origin ||
			!SetXmlProperty(origin, "y", FormatMetadataDouble(southMercator)) ||
			!SetXmlProperty(origin, "x", FormatMetadataDouble(westMercator)))
		{
			return {};
		}

		xmlNodePtr tileFormat = AddXmlChild(root, "TileFormat");
		if (!tileFormat ||
			!SetXmlProperty(tileFormat, "width", std::to_string(metadata.tileSize)) ||
			!SetXmlProperty(tileFormat, "height", std::to_string(metadata.tileSize)) ||
			!SetXmlProperty(tileFormat, "mime-type", "image/png") ||
			!SetXmlProperty(tileFormat, "extension", "png"))
		{
			return {};
		}

		xmlNodePtr tileSets = AddXmlChild(root, "TileSets");
		if (!tileSets || !SetXmlProperty(tileSets, "profile", "global-mercator"))
		{
			return {};
		}

		double unitsPerPixel = 156543.033928041;
		for (uint32_t zoom = 0; zoom <= metadata.maxZoom; zoom++)
		{
			xmlNodePtr tileSet = AddXmlChild(tileSets, "TileSet");
			if (!tileSet ||
				!SetXmlProperty(tileSet, "units-per-pixel", FormatMetadataDouble(unitsPerPixel)) ||
				!SetXmlProperty(tileSet, "order", std::to_string(zoom)))
			{
				return {};
			}
			unitsPerPixel /= 2.;
		}

		xmlChar* rawBuffer = nullptr;
		int bufferSize = 0;
		xmlDocDumpFormatMemory(document.get(), &rawBuffer, &bufferSize, 1);
		XmlBufferPtr buffer(rawBuffer);
		if (!buffer || bufferSize <= 0)
		{
			return {};
		}

		return std::string(
			reinterpret_cast<const char*>(buffer.get()), static_cast<size_t>(bufferSize));
	}

	bool WriteImage(const std::filesystem::path& path, const cv::Mat& image)
	{
		if (image.empty())
		{
			return false;
		}

		std::error_code createDirectoryError;
		std::filesystem::create_directories(path.parent_path(), createDirectoryError);
		if (createDirectoryError)
		{
			return false;
		}
		std::string extension = path.extension().string();
		if (extension.empty())
		{
			extension = ".png";
		}

		std::vector<unsigned char> data;
		if (!cv::imencode(extension, image, data))
		{
			return false;
		}

		std::ofstream fileStream(path, std::ios::binary);
		if (!fileStream.is_open())
		{
			return false;
		}

		fileStream.write(reinterpret_cast<const char*>(data.data()), data.size());
		const bool writeSuccess = fileStream.good();
		fileStream.close();
		return writeSuccess && !fileStream.fail();
	}
}

void DownTool::DownToolBase::WaitDown()
{
	// 1. 执行 I/O 操作（关键！）
	int still_running = 1;
	if (curl_multi_perform(curlMulti, &still_running) != CURLM_OK)
	{
		fmt::print("CURL multi perform error\n");
		return;
	}

	// 2. 检查是否有已完成的任务
	CURLMsg* msg;
	int msgs_left;
	bool hasProgressUpdate = false;
	while ((msg = curl_multi_info_read(curlMulti, &msgs_left)))
	{
		if (bStop) break;

		if (msg->msg == CURLMSG_DONE)
		{
			CURL* easy = msg->easy_handle;
			auto handleIt = curlEasyHandles.find(easy);
			Tile tile;
			std::filesystem::path tilePath;
			std::string url;
			CurlRequestType requestType = CurlRequestType::Tile;
			uint32_t repairTargetZoom = 0;
			bool hasContext = false;
			bool responseHeaderUnavailable = false;
			if (handleIt != curlEasyHandles.end())
			{
				tile = handleIt->second.tile;
				tilePath = handleIt->second.filePath;
				url = handleIt->second.url;
				requestType = handleIt->second.requestType;
				repairTargetZoom = handleIt->second.repairTargetZoom;
				handleIt->second.fileStream.close();
				responseHeaderUnavailable = IsUnavailableTileResponseHeader(
					handleIt->second.responseHeaders, tile);
				hasContext = true;
			}

			// 可选：检查结果
			if (msg->data.result != CURLE_OK)
			{
				if (hasContext)
				{
					fmt::print("\nRequest failed: {}, url: {}\n",
						curl_easy_strerror(msg->data.result), url);
				}
				else
				{
					fmt::print("\nRequest failed: {}\n", curl_easy_strerror(msg->data.result));
				}
			}
			// 清理 easy handle（重要！）
			curl_multi_remove_handle(curlMulti, easy);
			curl_easy_cleanup(easy);
			if (handleIt != curlEasyHandles.end())
			{
				curlEasyHandles.erase(handleIt);
			}
			if (!hasContext)
			{
				continue;
			}

			if (requestType == CurlRequestType::Tile)
			{
				if (msg->data.result == CURLE_OK &&
					(responseHeaderUnavailable || IsUnavailableTileFile(tilePath, tile)))
				{
					if (responseHeaderUnavailable)
					{
						RemoveFileIfExists(tilePath);
					}
					if (!RepairTileFromLowerZoom(tile, tilePath))
					{
						fmt::print("\n无法从低级别瓦片修复占位图: {}\n", PathToUtf8String(tilePath));
					}
				}

				DowningCount += 1;
				hasProgressUpdate = true;
				continue;
			}

			if (msg->data.result == CURLE_OK && responseHeaderUnavailable)
			{
				RemoveFileIfExists(tilePath);
			}

			if (msg->data.result == CURLE_OK &&
				!responseHeaderUnavailable &&
				IsValidImageFile(tilePath) &&
				!IsUnavailableTileFile(tilePath, tile))
			{
				if (RepairTilesFromParent(tile, repairTargetZoom))
				{
					continue;
				}

				fmt::print("\n无法完整批量生成高级别修复瓦片: {}\n", PathToUtf8String(tilePath));
			}

			if (tile.Zoom > GetMinFallbackZoom() && repairTargetZoom > tile.Zoom)
			{
				const Tile retryTile = GetFirstChildTile(tile, repairTargetZoom);
				if (!RepairTileFromLowerZoom(
					retryTile, GetTileFilePath(retryTile), tile.Zoom - 1))
				{
					fmt::print("\n无法继续降级修复占位图: {}\n", PathToUtf8String(tilePath));
				}
			}
		}
	}

	if (hasProgressUpdate)
	{
		fmt::print("\r下载进度: {}/{}", GetDowningCount(), TotalDownCount);
		return;
	}

	// 3. 等待 socket 事件（避免 busy loop）
	if (!curlEasyHandles.empty())
	{
		CURLMcode  mc = curl_multi_wait(curlMulti, nullptr, 0, 100, nullptr);
		if (mc != CURLM_OK)
		{
			fmt::print("curl_multi_wait failed: {}\n", curl_multi_strerror(mc));
		}
	}
}

DownTool::DownToolBase::~DownToolBase()
{
	StopDown();

	if (curlMulti)
	{
		curl_multi_cleanup(curlMulti);
		curlMulti = nullptr;
	}
	if (share)
	{
		curl_share_cleanup(share);
		share = nullptr;
	}
}

void DownTool::DownToolBase::StartDown()
{
	if (bDowning)
	{
		fmt::print("下载中...无法重复进行下载");
		return;
	}

	bDowning = true;
	bStop = false;
	DowningCount = 0;
	TotalDownCount = 0;

	curlMulti = curl_multi_init();

	// 初始化共享句柄，设置共享DNS缓存
	share = curl_share_init();
	curl_share_setopt(share, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);

	CalculateDownRange();
	std::vector<Tile> targetTiles;
	BuildTargetTiles(targetTiles);
	TotalDownCount = static_cast<uint32_t>(targetTiles.size());

	fmt::print("下载范围：\n左上角经纬度：({}, {})\n右下角经纬度：({}, {})\n左上角瓦片坐标：({}, {})\n右下角瓦片坐标：({}, {})\n缓存路径：{}\n总下载切片数量预估：{}\n",
		MinLonLat.lon, MaxLonLat.lat, MaxLonLat.lon, MinLonLat.lat,
		MinTile.X, MinTile.Y, MaxTile.X, MaxTile.Y, CachePath, TotalDownCount);

	std::vector<Tile> pendingTiles =
		ScanCacheAndCollectPendingTiles(targetTiles, "下载前缓存检测", true);

	for (const Tile& tile : pendingTiles)
	{
		if (bStop) break;
		DownloadTile(tile.X, tile.Y);
	}

	// 等待所有下载完成
	while (curlEasyHandles.size() > 0)
	{
		if (bStop) break;
		WaitDown();
	}
	fmt::print("\n");

	std::vector<Tile> failedTiles =
		ScanCacheAndCollectPendingTiles(targetTiles, "下载后缓存检测", false);
	if (!failedTiles.empty())
	{
		fmt::print("下载结束后仍有 {} 个瓦片不可用，请查看上面的缓存检测明细。\n",
			failedTiles.size());
	}
	else if (!bStop && (PostProcessEnabled || PostProcessMergedImageEnabled))
	{
		RunPostProcess();
	}

	/*for (auto& d : downTileCount)
	{
		fmt::print("URL前缀：{}，下载次数：{}\n", d.first, d.second);
	}*/

	bDowning = false;
}

void DownTool::DownToolBase::BuildTargetTiles(std::vector<Tile>& targetTiles)
{
	targetTiles.clear();
	const uint64_t xCount = static_cast<uint64_t>(MaxTile.X) - MinTile.X + 1;
	const uint64_t yCount = static_cast<uint64_t>(MaxTile.Y) - MinTile.Y + 1;
	const uint64_t estimatedTileCount = xCount * yCount * (IsTileOffset() ? 4 : 1);
	targetTiles.reserve(static_cast<size_t>(estimatedTileCount));

	std::unordered_set<Tile, TileHash, TileEqual> uniqueTiles;
	uniqueTiles.reserve(static_cast<size_t>(estimatedTileCount));

	auto addTile = [&](const Tile& tile)
	{
		if (uniqueTiles.insert(tile).second)
		{
			targetTiles.push_back(tile);
		}
	};

	for (uint32_t xBase = MinTile.X; xBase <= MaxTile.X; xBase++)
	{
		if (bStop) break;
		for (uint32_t yBase = MinTile.Y; yBase <= MaxTile.Y; yBase++)
		{
			if (bStop) break;

			if (IsTileOffset())
			{
				const TileRange offsetTiles = GetOffsetTiles(Tile(xBase, yBase, Zoom));
				for (uint32_t x = offsetTiles.MinTile.X; x <= offsetTiles.MaxTile.X; x++)
				{
					for (uint32_t y = offsetTiles.MinTile.Y; y <= offsetTiles.MaxTile.Y; y++)
					{
						addTile(Tile(x, y, Zoom));
					}
				}
			}
			else
			{
				addTile(Tile(xBase, yBase, Zoom));
			}
		}
	}
}

void DownTool::DownToolBase::BuildRequestedTiles(std::vector<Tile>& targetTiles) const
{
	targetTiles.clear();
	const uint64_t xCount = static_cast<uint64_t>(MaxTile.X) - MinTile.X + 1;
	const uint64_t yCount = static_cast<uint64_t>(MaxTile.Y) - MinTile.Y + 1;
	targetTiles.reserve(static_cast<size_t>(xCount * yCount));

	for (uint32_t x = MinTile.X; x <= MaxTile.X; x++)
	{
		for (uint32_t y = MinTile.Y; y <= MaxTile.Y; y++)
		{
			targetTiles.emplace_back(x, y, Zoom);
		}
	}
}

void DownTool::DownToolBase::BuildRequestedTilesAtZoom(
	uint32_t zoom, std::vector<Tile>& targetTiles) const
{
	targetTiles.clear();
	if (zoom > Zoom)
	{
		return;
	}

	const uint32_t zoomOffset = Zoom - zoom;
	if (zoomOffset >= 31)
	{
		return;
	}

	const uint64_t scale = uint64_t{ 1 } << zoomOffset;
	const uint32_t minX = static_cast<uint32_t>(static_cast<uint64_t>(MinTile.X) / scale);
	const uint32_t maxX = static_cast<uint32_t>(static_cast<uint64_t>(MaxTile.X) / scale);
	const uint32_t minY = static_cast<uint32_t>(static_cast<uint64_t>(MinTile.Y) / scale);
	const uint32_t maxY = static_cast<uint32_t>(static_cast<uint64_t>(MaxTile.Y) / scale);
	const uint64_t xCount = static_cast<uint64_t>(maxX) - minX + 1;
	const uint64_t yCount = static_cast<uint64_t>(maxY) - minY + 1;
	targetTiles.reserve(static_cast<size_t>(xCount * yCount));

	for (uint32_t x = minX; x <= maxX; x++)
	{
		for (uint32_t y = minY; y <= maxY; y++)
		{
			targetTiles.emplace_back(x, y, zoom);
		}
	}
}

DownTool::TileCacheState DownTool::DownToolBase::GetTileCacheState(
	const Tile& tile) const
{
	const std::filesystem::path tilePath = GetTileFilePath(tile);
	std::error_code existsError;
	const bool tileExists = std::filesystem::exists(tilePath, existsError);
	if (existsError)
	{
		return TileCacheState::Invalid;
	}

	if (!tileExists)
	{
		return TileCacheState::Missing;
	}

	if (IsUnavailableTileFile(tilePath, tile))
	{
		return TileCacheState::Unavailable;
	}

	if (IsValidImageFile(tilePath))
	{
		return TileCacheState::Ready;
	}

	return TileCacheState::Invalid;
}

std::vector<DownTool::Tile> DownTool::DownToolBase::ScanCacheAndCollectPendingTiles(
	const std::vector<Tile>& targetTiles,
	const std::string& phaseName,
	bool updateProgress)
{
	std::vector<TileCacheState> states(targetTiles.size(), TileCacheState::Missing);
	fmt::print("{}: 开始扫描 {} 个瓦片\n", phaseName, targetTiles.size());

	const uint32_t plannedWorkerCount = GetCacheScanWorkerCount(targetTiles.size());
	std::atomic_size_t nextTileIndex = 0;
	std::atomic_size_t scannedTileCount = 0;
	std::atomic_uint32_t scanErrorCount = 0;
	std::mutex scanErrorMutex;
	std::string firstScanErrorMessage;
	std::vector<std::thread> scanWorkers;
	scanWorkers.reserve(plannedWorkerCount);

	auto recordScanError = [&](const Tile& tile, const char* message)
	{
		if (scanErrorCount.fetch_add(1, std::memory_order_relaxed) == 0)
		{
			std::lock_guard<std::mutex> lock(scanErrorMutex);
			firstScanErrorMessage = fmt::format(
				"z{} ({}, {}) {}",
				tile.Zoom,
				tile.X,
				tile.Y,
				message);
		}
	};

	auto scanTile = [&](size_t index)
	{
		try
		{
			states[index] = GetTileCacheState(targetTiles[index]);
		}
		catch (const std::exception& exception)
		{
			states[index] = TileCacheState::Invalid;
			recordScanError(targetTiles[index], exception.what());
		}
		catch (...)
		{
			states[index] = TileCacheState::Invalid;
			recordScanError(targetTiles[index], "未知异常");
		}
	};

	for (uint32_t workerIndex = 0; workerIndex < plannedWorkerCount; workerIndex++)
	{
		(void)workerIndex;
		try
		{
			scanWorkers.emplace_back(
				[&]()
				{
					for (;;)
					{
						const size_t blockBegin = nextTileIndex.fetch_add(
							CacheScanBatchSize, std::memory_order_relaxed);
						if (blockBegin >= targetTiles.size())
						{
							break;
						}

						const size_t blockEnd =
							std::min(blockBegin + CacheScanBatchSize, targetTiles.size());
						for (size_t index = blockBegin; index < blockEnd; index++)
						{
							scanTile(index);
						}
						scannedTileCount.fetch_add(
							blockEnd - blockBegin, std::memory_order_relaxed);
					}
				});
		}
		catch (const std::exception& exception)
		{
			fmt::print("\n{}: 创建缓存检测 worker 失败: {}\n",
				phaseName, exception.what());
			break;
		}
	}

	if (scanWorkers.empty())
	{
		for (size_t index = 0; index < targetTiles.size(); index++)
		{
			scanTile(index);
			scannedTileCount.fetch_add(1, std::memory_order_relaxed);
		}
	}

	size_t lastPrintedCount = 0;
	while (lastPrintedCount < targetTiles.size())
	{
		const size_t currentCount = scannedTileCount.load(std::memory_order_relaxed);
		if (currentCount != lastPrintedCount)
		{
			fmt::print("\r{}: {}/{}，worker {}",
				phaseName, currentCount, targetTiles.size(), scanWorkers.size());
			lastPrintedCount = currentCount;
		}

		if (currentCount >= targetTiles.size())
		{
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
	}

	for (std::thread& worker : scanWorkers)
	{
		if (worker.joinable())
		{
			worker.join();
		}
	}
	fmt::print("\r{}: {}/{}，worker {}\n",
		phaseName, targetTiles.size(), targetTiles.size(), scanWorkers.size());
	if (scanErrorCount.load(std::memory_order_relaxed) > 0)
	{
		fmt::print("{}: 缓存检测异常 {} 次，首个异常: {}\n",
			phaseName,
			scanErrorCount.load(std::memory_order_relaxed),
			firstScanErrorMessage);
	}

	uint32_t readyCount = 0;
	uint32_t missingCount = 0;
	uint32_t unavailableCount = 0;
	uint32_t invalidCount = 0;
	std::vector<Tile> pendingTiles;
	pendingTiles.reserve(targetTiles.size());

	std::vector<size_t> issueSampleIndexes;
	issueSampleIndexes.reserve(MaxCacheIssueSampleCount);
	for (size_t index = 0; index < targetTiles.size(); index++)
	{
		switch (states[index])
		{
		case TileCacheState::Ready:
			readyCount++;
			break;
		case TileCacheState::Missing:
			missingCount++;
			pendingTiles.push_back(targetTiles[index]);
			break;
		case TileCacheState::Unavailable:
			unavailableCount++;
			pendingTiles.push_back(targetTiles[index]);
			break;
		case TileCacheState::Invalid:
			invalidCount++;
			pendingTiles.push_back(targetTiles[index]);
			break;
		default:
			pendingTiles.push_back(targetTiles[index]);
			break;
		}

		if (states[index] != TileCacheState::Ready &&
			issueSampleIndexes.size() < MaxCacheIssueSampleCount)
		{
			issueSampleIndexes.push_back(index);
		}
	}

	if (updateProgress)
	{
		DowningCount = static_cast<std::int32_t>(readyCount);
	}

	fmt::print("{}完成：可用 {}，缺失 {}，占位图 {}，无效文件 {}，待处理 {}\n",
		phaseName, readyCount, missingCount, unavailableCount, invalidCount,
		pendingTiles.size());
	if (!issueSampleIndexes.empty())
	{
		fmt::print("{}问题样例（最多 {} 个）：\n",
			phaseName, MaxCacheIssueSampleCount);
		for (size_t index : issueSampleIndexes)
		{
			const Tile& tile = targetTiles[index];
			fmt::print("  [{}] z{} ({}, {}) {}\n",
				GetTileCacheStateText(states[index]),
				tile.Zoom,
				tile.X,
				tile.Y,
				PathToUtf8String(GetTileFilePath(tile)));
		}
	}

	if (updateProgress)
	{
		fmt::print("\r下载进度: {}/{}", GetDowningCount(), TotalDownCount);
	}

	return pendingTiles;
}

void DownTool::DownToolBase::DownloadTile(uint32_t x, uint32_t y)
{
	// 等待至少一个下载完成
	while (curlEasyHandles.size() >= GetEffectiveQueueCount(MaxDownQueue))
	{
		WaitDown();
	}

	Tile tile(x, y, GetZoom());
	std::filesystem::path nativePath = GetTileFilePath(tile);

	if (IsTileDownloading(tile))
	{
		DowningCount++;
		fmt::print("\r下载进度: {}/{}", GetDowningCount(), TotalDownCount);
		return;
	}

	//如果文件存在，检测是不是一个有效的图片文件，如果是的话就跳过下载，如果不是的话就删除这个文件重新下载
	std::error_code existsError;
	const bool tileExists = std::filesystem::exists(nativePath, existsError);
	if (existsError)
	{
		fmt::print("\n检测缓存文件失败，将重新下载: {}\n", PathToUtf8String(nativePath));
	}
	if (tileExists)
	{
		if (IsUnavailableTileFile(nativePath, tile))
		{
			if (!RepairTileFromLowerZoom(tile, nativePath))
			{
				fmt::print("\n无法从低级别瓦片修复占位图: {}\n", PathToUtf8String(nativePath));
			}

			DowningCount++;
			fmt::print("\r下载进度: {}/{}", GetDowningCount(), TotalDownCount);
			return;
		}
		else if (IsValidImageFile(nativePath))
		{
			DowningCount++;
			fmt::print("\r下载进度: {}/{}", GetDowningCount(), TotalDownCount);
			return;
		}
	}

	if (RepairTileFromExistingLowerZoom(tile))
	{
		DowningCount++;
		fmt::print("\r下载进度: {}/{}", GetDowningCount(), TotalDownCount);
		return;
	}

	if (IsCoveredByPendingLowerZoomRepair(tile))
	{
		DowningCount++;
		fmt::print("\r下载进度: {}/{}", GetDowningCount(), TotalDownCount);
		return;
	}

	if (!QueueTileDownload(tile, nativePath, CurlRequestType::Tile, tile.Zoom))
	{
		DowningCount++;
		fmt::print("\r下载进度: {}/{}", GetDowningCount(), TotalDownCount);
	}
}

std::filesystem::path DownTool::DownToolBase::GetTileFilePath(const Tile& tile) const
{
	std::string filePath = fmt::format("{}/{}/{}/{}.png", CachePath, tile.Zoom, tile.X, tile.Y);
	return std::filesystem::u8path(filePath);
}

std::filesystem::path DownTool::DownToolBase::GetPostProcessRootPath() const
{
	if (!PostProcessOutputPath.empty())
	{
		return std::filesystem::u8path(PostProcessOutputPath);
	}

	return std::filesystem::u8path(fmt::format("{}/Google_墨卡托", CachePath));
}

std::filesystem::path DownTool::DownToolBase::GetPostProcessTileFilePath(
	const Tile& tile) const
{
	return GetPostProcessRootPath() /
		std::to_string(tile.Zoom) /
		std::to_string(tile.X) /
		fmt::format("{}.png", tile.Y);
}

std::filesystem::path DownTool::DownToolBase::GetPostProcessMergedImagePath() const
{
	if (!PostProcessMergedImageOutputPath.empty())
	{
		return std::filesystem::u8path(PostProcessMergedImageOutputPath);
	}

	return GetPostProcessRootPath() / "merged.png";
}

bool DownTool::DownToolBase::QueueTileDownload(
	const Tile& tile,
	const std::filesystem::path& tilePath,
	CurlRequestType requestType,
	uint32_t repairTargetZoom)
{
	if (IsTileDownloading(tile))
	{
		return true;
	}

	std::error_code createDirectoryError;
	std::filesystem::create_directories(tilePath.parent_path(), createDirectoryError);
	if (createDirectoryError)
	{
		return false;
	}

	CurlMultiHandle multiHandle;
	multiHandle.fileStream.open(tilePath, std::ios::binary);
	if (!multiHandle.fileStream.is_open())
	{
		fmt::print("无法打开文件进行写入，切片将会被跳过，解决文件占用问题后再重新下载一遍即可修复：{}\n", PathToUtf8String(tilePath));
		return false;
	}
	multiHandle.owner = this;
	multiHandle.requestType = requestType;
	multiHandle.tile = tile;
	multiHandle.repairTargetZoom = repairTargetZoom;
	multiHandle.filePath = tilePath;

	CURL* easy = curl_easy_init();
	if (!easy)
	{
		fmt::print("无法初始化 CURL easy handle\n");
		return false;
	}

	std::string url = MakeUri(multiHandle.tile);
	multiHandle.url = url;
	//downTileCount[url.substr(0, 20)]++;
	curlEasyHandles[easy] = std::move(multiHandle);
	//避免高速下载被服务器拒绝，设置最大连接数为6（根据服务器限制调整）
	//curl_easy_setopt(easy, CURLOPT_MAXCONNECTS, 2L);
	if (share)
	{
		CURLcode result = curl_easy_setopt(easy, CURLOPT_SHARE, share);
		if (result != CURLE_OK)
		{
			fmt::print("设置 CURL 共享 DNS 失败: {}\n", curl_easy_strerror(result));
		}
	}
	curl_easy_setopt(easy, CURLOPT_URL, url.c_str());
	curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, WriteTileData);
	curl_easy_setopt(easy, CURLOPT_WRITEDATA, &curlEasyHandles[easy]);
	curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, WriteTileHeaderData);
	curl_easy_setopt(easy, CURLOPT_HEADERDATA, &curlEasyHandles[easy]);
	if (curl_multi_add_handle(curlMulti, easy) != CURLM_OK)
	{
		fmt::print("无法将 CURL easy handle 添加到 multi handle\n");
		curlEasyHandles[easy].fileStream.close();
		curlEasyHandles.erase(easy);
		curl_easy_cleanup(easy);
		return false;
	}

	return true;
}

bool DownTool::DownToolBase::IsTileDownloading(const Tile& tile) const
{
	for (const auto& [easy, context] : curlEasyHandles)
	{
		(void)easy;
		if (IsSameTile(context.tile, tile))
		{
			return true;
		}
	}

	return false;
}

bool DownTool::DownToolBase::IsCoveredByPendingLowerZoomRepair(const Tile& tile) const
{
	for (const auto& [easy, context] : curlEasyHandles)
	{
		(void)easy;
		if (context.requestType != CurlRequestType::RepairParent ||
			context.repairTargetZoom != tile.Zoom ||
			context.tile.Zoom >= tile.Zoom)
		{
			continue;
		}

		const uint32_t zoomOffset = tile.Zoom - context.tile.Zoom;
		if (zoomOffset >= 31)
		{
			continue;
		}

		const uint64_t scale = uint64_t{ 1 } << zoomOffset;
		const uint64_t minX = static_cast<uint64_t>(context.tile.X) * scale;
		const uint64_t minY = static_cast<uint64_t>(context.tile.Y) * scale;
		const uint64_t maxX = minX + scale;
		const uint64_t maxY = minY + scale;
		const uint64_t tileX = tile.X;
		const uint64_t tileY = tile.Y;
		if (minX <= tileX && tileX < maxX && minY <= tileY && tileY < maxY)
		{
			return true;
		}
	}

	return false;
}

uint32_t DownTool::DownToolBase::CancelPendingChildDownloads(
	const Tile& parentTile, uint32_t childZoom)
{
	if (childZoom <= parentTile.Zoom)
	{
		return 0;
	}

	const uint32_t zoomOffset = childZoom - parentTile.Zoom;
	if (zoomOffset >= 31)
	{
		return 0;
	}

	const uint64_t scale = uint64_t{ 1 } << zoomOffset;
	const uint64_t minX = static_cast<uint64_t>(parentTile.X) * scale;
	const uint64_t minY = static_cast<uint64_t>(parentTile.Y) * scale;
	const uint64_t maxX = minX + scale;
	const uint64_t maxY = minY + scale;

	std::vector<CURL*> handlesToCancel;
	for (const auto& [easy, context] : curlEasyHandles)
	{
		const uint64_t tileX = context.tile.X;
		const uint64_t tileY = context.tile.Y;
		if (context.requestType == CurlRequestType::Tile &&
			context.tile.Zoom == childZoom &&
			minX <= tileX && tileX < maxX &&
			minY <= tileY && tileY < maxY)
		{
			handlesToCancel.push_back(easy);
		}
	}

	uint32_t canceledCount = 0;
	for (CURL* easy : handlesToCancel)
	{
		auto handleIt = curlEasyHandles.find(easy);
		if (handleIt == curlEasyHandles.end())
		{
			continue;
		}

		handleIt->second.fileStream.close();
		curl_multi_remove_handle(curlMulti, easy);
		curl_easy_cleanup(easy);
		curlEasyHandles.erase(handleIt);
		canceledCount++;
	}

	return canceledCount;
}

bool DownTool::DownToolBase::RepairTileFromLowerZoom(
	const Tile& tile, const std::filesystem::path& tilePath)
{
	if (tile.Zoom == 0)
	{
		return false;
	}

	return RepairTileFromLowerZoom(tile, tilePath, tile.Zoom - 1);
}

bool DownTool::DownToolBase::RepairTileFromLowerZoom(
	const Tile& tile,
	const std::filesystem::path& tilePath,
	uint32_t startParentZoom)
{
	(void)tilePath;

	if (tile.Zoom == 0 || startParentZoom >= tile.Zoom)
	{
		return false;
	}

	if (startParentZoom < GetMinFallbackZoom())
	{
		return false;
	}

	for (uint32_t parentZoom = startParentZoom;; parentZoom--)
	{
		const uint32_t zoomOffset = tile.Zoom - parentZoom;
		if (zoomOffset >= 31)
		{
			return false;
		}

		const uint32_t scale = 1u << zoomOffset;
		const Tile parentTile(tile.X / scale, tile.Y / scale, parentZoom);
		const std::filesystem::path parentPath = GetTileFilePath(parentTile);
		std::error_code parentExistsError;
		const bool parentExists = std::filesystem::exists(parentPath, parentExistsError);
		if (parentExistsError)
		{
			if (parentZoom == GetMinFallbackZoom())
			{
				break;
			}
			continue;
		}
		if (parentExists && IsUnavailableTileFile(parentPath, parentTile))
		{
			if (parentZoom == GetMinFallbackZoom())
			{
				break;
			}
			continue;
		}

		if (!parentExists || !IsValidImageFile(parentPath))
		{
			if (QueueTileDownload(
				parentTile, parentPath, CurlRequestType::RepairParent, tile.Zoom))
			{
				return true;
			}

			if (parentZoom == GetMinFallbackZoom())
			{
				break;
			}
			continue;
		}

		if (RepairTilesFromParent(parentTile, tile.Zoom))
		{
			return true;
		}

		if (parentZoom == GetMinFallbackZoom())
		{
			break;
		}
	}

	return false;
}

bool DownTool::DownToolBase::RepairTileFromExistingLowerZoom(const Tile& tile)
{
	if (tile.Zoom == 0)
	{
		return false;
	}

	return RepairTileFromExistingLowerZoom(tile, tile.Zoom - 1);
}

bool DownTool::DownToolBase::RepairTileFromExistingLowerZoom(
	const Tile& tile,
	uint32_t startParentZoom)
{
	if (tile.Zoom == 0 || startParentZoom >= tile.Zoom)
	{
		return false;
	}

	if (startParentZoom < GetMinFallbackZoom())
	{
		return false;
	}

	for (uint32_t parentZoom = startParentZoom;; parentZoom--)
	{
		const uint32_t zoomOffset = tile.Zoom - parentZoom;
		if (zoomOffset >= 31)
		{
			return false;
		}

		const uint32_t scale = 1u << zoomOffset;
		const Tile parentTile(tile.X / scale, tile.Y / scale, parentZoom);
		const std::filesystem::path parentPath = GetTileFilePath(parentTile);
		std::error_code parentExistsError;
		const bool parentExists = std::filesystem::exists(parentPath, parentExistsError);
		if (!parentExistsError &&
			parentExists &&
			IsValidImageFile(parentPath) &&
			!IsUnavailableTileFile(parentPath, parentTile) &&
			RepairTilesFromParent(parentTile, tile.Zoom))
		{
			return true;
		}

		if (parentZoom == GetMinFallbackZoom())
		{
			break;
		}
	}

	return false;
}

bool DownTool::DownToolBase::RepairTilesFromParent(
	const Tile& parentTile, uint32_t targetZoom)
{
	if (targetZoom <= parentTile.Zoom)
	{
		return false;
	}

	const uint32_t zoomOffset = targetZoom - parentTile.Zoom;
	if (zoomOffset >= 31)
	{
		return false;
	}

	const uint32_t scale = 1u << zoomOffset;
	const uint64_t batchTileCount = static_cast<uint64_t>(scale) * scale;
	if (batchTileCount > MaxBatchRepairedTileCount)
	{
		return false;
	}

	const std::filesystem::path parentPath = GetTileFilePath(parentTile);
	if (!IsValidImageFile(parentPath) || IsUnavailableTileFile(parentPath, parentTile))
	{
		return false;
	}

	cv::Mat parentImage = ReadImage(parentPath);
	if (parentImage.empty())
	{
		return false;
	}

	const int cropWidth = parentImage.cols / static_cast<int>(scale);
	const int cropHeight = parentImage.rows / static_cast<int>(scale);
	if (cropWidth <= 0 || cropHeight <= 0)
	{
		return false;
	}

	auto writeRepairedTile = [&](const Tile& childTile, uint32_t childX, uint32_t childY)
		-> TileRepairWriteResult
	{
		const std::filesystem::path childPath = GetTileFilePath(childTile);
		std::error_code childExistsError;
		const bool childExists = std::filesystem::exists(childPath, childExistsError);
		if (!childExistsError && childExists &&
			IsValidImageFile(childPath) &&
			!IsUnavailableTileFile(childPath, childTile))
		{
			return TileRepairWriteResult::Existing;
		}

		const int cropX = static_cast<int>(childX) * cropWidth;
		const int cropY = static_cast<int>(childY) * cropHeight;
		if (cropX + cropWidth > parentImage.cols || cropY + cropHeight > parentImage.rows)
		{
			return TileRepairWriteResult::Failed;
		}

		cv::Mat cropped = parentImage(cv::Rect(cropX, cropY, cropWidth, cropHeight));
		cv::Mat resized;
		cv::resize(cropped, resized, parentImage.size(), 0., 0., cv::INTER_CUBIC);
		if (!WriteImage(childPath, resized))
		{
			return TileRepairWriteResult::Failed;
		}

		return TileRepairWriteResult::Written;
	};

	const uint32_t canceledTileCount = CancelPendingChildDownloads(parentTile, targetZoom);
	DowningCount += canceledTileCount;

	std::vector<TileRepairTask> repairTasks;
	repairTasks.reserve(static_cast<size_t>(batchTileCount));
	for (uint32_t childX = 0; childX < scale; childX++)
	{
		for (uint32_t childY = 0; childY < scale; childY++)
		{
			const Tile childTile(
				parentTile.X * scale + childX,
				parentTile.Y * scale + childY,
				targetZoom);
			repairTasks.push_back(TileRepairTask{ childTile, childX, childY });
		}
	}

	std::atomic_uint32_t atomicReadyTileCount = 0;
	std::atomic_uint32_t atomicRepairedTileCount = 0;
	ParallelForIndex(
		repairTasks.size(),
		MaxDownQueue,
		[&](size_t taskIndex)
		{
			const TileRepairTask& task = repairTasks[taskIndex];
			TileRepairWriteResult result = TileRepairWriteResult::Failed;
			try
			{
				result = writeRepairedTile(task.childTile, task.childX, task.childY);
			}
			catch (const std::exception&)
			{
				result = TileRepairWriteResult::Failed;
			}
			catch (...)
			{
				result = TileRepairWriteResult::Failed;
			}
			if (result != TileRepairWriteResult::Failed)
			{
				atomicReadyTileCount.fetch_add(1, std::memory_order_relaxed);
			}
			if (result == TileRepairWriteResult::Written)
			{
				atomicRepairedTileCount.fetch_add(1, std::memory_order_relaxed);
			}
		});

	const uint32_t readyTileCount = atomicReadyTileCount.load(std::memory_order_relaxed);
	const uint32_t repairedTileCount =
		atomicRepairedTileCount.load(std::memory_order_relaxed);

	fmt::print("\n使用低级别瓦片批量修复占位图: z{} ({}, {}) -> z{}，可用 {} / {} 张，写入 {} 张\n",
		parentTile.Zoom, parentTile.X, parentTile.Y, targetZoom,
		readyTileCount, batchTileCount, repairedTileCount);
	return readyTileCount == batchTileCount;
}

void DownTool::DownToolBase::RunPostProcess()
{
	const auto postProcessStartTime = std::chrono::steady_clock::now();
	std::vector<Tile> targetTiles;
	BuildRequestedTiles(targetTiles);
	if (targetTiles.empty())
	{
		fmt::print("后处理跳过：没有可处理的目标瓦片。\n");
		return;
	}

	fmt::print("开始瓦片后处理，输出目录：{}\n", PathToUtf8String(GetPostProcessRootPath()));
	bool maxZoomTilesReady = false;
	if (PostProcessEnabled)
	{
		auto phaseStartTime = std::chrono::steady_clock::now();
		maxZoomTilesReady = GeneratePostProcessMaxZoomTiles(targetTiles);
		fmt::print("最高层级后处理耗时：{:.2f}s\n", SecondsSince(phaseStartTime));
		if (maxZoomTilesReady)
		{
			phaseStartTime = std::chrono::steady_clock::now();
			GeneratePostProcessLowerZoomTiles(targetTiles);
			fmt::print("低层级金字塔后处理耗时：{:.2f}s\n", SecondsSince(phaseStartTime));
			phaseStartTime = std::chrono::steady_clock::now();
			if (!WritePostProcessMetadata())
			{
				fmt::print("后处理警告：元数据写入失败。\n");
			}
			fmt::print("元数据写入耗时：{:.2f}s\n", SecondsSince(phaseStartTime));
		}
		else
		{
			fmt::print("后处理警告：最高层级瓦片生成不完整，跳过低层级和元数据生成。\n");
		}
	}

	if (PostProcessMergedImageEnabled)
	{
		const auto phaseStartTime = std::chrono::steady_clock::now();
		const bool postProcessTilesAvailable = PostProcessEnabled && maxZoomTilesReady;
		if (!GeneratePostProcessMergedImage(postProcessTilesAvailable))
		{
			fmt::print("后处理警告：合并大图生成失败或不完整。\n");
		}
		fmt::print("合并大图后处理耗时：{:.2f}s\n", SecondsSince(phaseStartTime));
	}

	fmt::print("瓦片后处理完成，总耗时：{:.2f}s\n", SecondsSince(postProcessStartTime));
}

bool DownTool::DownToolBase::GeneratePostProcessMaxZoomTiles(
	const std::vector<Tile>& targetTiles)
{
	std::atomic_uint32_t processedCount = 0;
	std::atomic_uint32_t skippedCount = 0;
	std::atomic_uint32_t failedCount = 0;

	ParallelForIndex(
		targetTiles.size(),
		MaxDownQueue,
		[&](size_t index)
		{
			const Tile& tile = targetTiles[index];
			const std::filesystem::path outputPath = GetPostProcessTileFilePath(tile);
			bool writeSuccess = false;
			std::error_code existsError;
			const bool outputExists = std::filesystem::exists(outputPath, existsError);
			const bool reusableOutput =
				!existsError &&
				outputExists &&
				IsValidImageFile(outputPath);
			if (reusableOutput)
			{
				writeSuccess = true;
				skippedCount.fetch_add(1, std::memory_order_relaxed);
			}
			else
			{
				try
				{
					writeSuccess = WritePostProcessMaxZoomTile(tile, outputPath);
				}
				catch (const std::exception&)
				{
					writeSuccess = false;
				}
				catch (...)
				{
					writeSuccess = false;
				}
			}

			if (!writeSuccess)
			{
				failedCount.fetch_add(1, std::memory_order_relaxed);
			}
			const uint32_t currentCount =
				processedCount.fetch_add(1, std::memory_order_relaxed) + 1;
			if (currentCount % 1024 == 0 || currentCount == targetTiles.size())
			{
				fmt::print("\r最高层级后处理: {}/{}",
					currentCount, targetTiles.size());
			}
		});

	fmt::print("\r最高层级后处理: {}/{}，跳过 {}，失败 {}\n",
		targetTiles.size(),
		targetTiles.size(),
		skippedCount.load(),
		failedCount.load());
	return failedCount.load() == 0;
}

uint32_t DownTool::DownToolBase::SelectPostProcessMergedImageSampleZoom() const
{
	const double targetResolution =
		static_cast<double>(std::max(1u, PostProcessMergedImageResolution));
	uint32_t bestZoom = Zoom;
	double bestScore = std::numeric_limits<double>::max();

	for (uint32_t zoom = 0; zoom <= Zoom; zoom++)
	{
		const uint32_t zoomOffset = Zoom - zoom;
		if (zoomOffset >= 31)
		{
			continue;
		}

		const uint64_t scale = uint64_t{ 1 } << zoomOffset;
		const uint64_t minX = static_cast<uint64_t>(MinTile.X) / scale;
		const uint64_t maxX = static_cast<uint64_t>(MaxTile.X) / scale;
		const uint64_t minY = static_cast<uint64_t>(MinTile.Y) / scale;
		const uint64_t maxY = static_cast<uint64_t>(MaxTile.Y) / scale;
		const uint64_t xCount = maxX - minX + 1;
		const uint64_t yCount = maxY - minY + 1;
		const double sourcePixels =
			static_cast<double>(std::max(xCount, yCount)) * TileSize;
		if (sourcePixels <= 0.)
		{
			continue;
		}

		const double score = std::abs(std::log(sourcePixels / targetResolution));
		if (score < bestScore || (score == bestScore && zoom < bestZoom))
		{
			bestScore = score;
			bestZoom = zoom;
		}
	}

	return bestZoom;
}

bool DownTool::DownToolBase::GeneratePostProcessTilesAtZoom(
	const std::vector<Tile>& targetTiles,
	uint32_t sourceZoom,
	const std::string& phaseName) const
{
	if (targetTiles.empty())
	{
		return true;
	}

	std::atomic_uint32_t processedCount = 0;
	std::atomic_uint32_t skippedCount = 0;
	std::atomic_uint32_t failedCount = 0;

	ParallelForIndex(
		targetTiles.size(),
		MaxDownQueue,
		[&](size_t index)
		{
			const Tile& tile = targetTiles[index];
			const std::filesystem::path outputPath = GetPostProcessTileFilePath(tile);
			bool writeSuccess = false;
			std::error_code existsError;
			const bool outputExists = std::filesystem::exists(outputPath, existsError);
			if (!existsError && outputExists && IsValidImageFile(outputPath))
			{
				writeSuccess = true;
				skippedCount.fetch_add(1, std::memory_order_relaxed);
			}
			else
			{
				try
				{
					writeSuccess =
						WritePostProcessTileFromSourceZoom(tile, outputPath, sourceZoom);
				}
				catch (const std::exception&)
				{
					writeSuccess = false;
				}
				catch (...)
				{
					writeSuccess = false;
				}
			}

			if (!writeSuccess)
			{
				failedCount.fetch_add(1, std::memory_order_relaxed);
			}
			const uint32_t currentCount =
				processedCount.fetch_add(1, std::memory_order_relaxed) + 1;
			if (currentCount % 1024 == 0 || currentCount == targetTiles.size())
			{
				fmt::print("\r{}: {}/{}",
					phaseName, currentCount, targetTiles.size());
			}
		});

	fmt::print("\r{}: {}/{}，跳过 {}，失败 {}\n",
		phaseName,
		targetTiles.size(),
		targetTiles.size(),
		skippedCount.load(),
		failedCount.load());
	return failedCount.load() == 0;
}

bool DownTool::DownToolBase::IsPostProcessLowerZoomTileFresh(const Tile& parentTile) const
{
	const std::filesystem::path parentPath = GetPostProcessTileFilePath(parentTile);
	if (!IsValidImageFile(parentPath))
	{
		return false;
	}

	std::error_code parentTimeError;
	const std::filesystem::file_time_type parentWriteTime =
		std::filesystem::last_write_time(parentPath, parentTimeError);
	if (parentTimeError)
	{
		return false;
	}

	bool hasChildFile = false;
	for (uint32_t childX = 0; childX < 2; childX++)
	{
		for (uint32_t childY = 0; childY < 2; childY++)
		{
			const Tile childTile(
				parentTile.X * 2 + childX,
				parentTile.Y * 2 + childY,
				parentTile.Zoom + 1);
			const std::filesystem::path childPath = GetPostProcessTileFilePath(childTile);
			std::error_code childExistsError;
			if (!std::filesystem::exists(childPath, childExistsError) || childExistsError)
			{
				continue;
			}
			if (!IsValidImageFile(childPath))
			{
				return false;
			}

			std::error_code childTimeError;
			const std::filesystem::file_time_type childWriteTime =
				std::filesystem::last_write_time(childPath, childTimeError);
			if (childTimeError || childWriteTime > parentWriteTime)
			{
				return false;
			}
			hasChildFile = true;
		}
	}

	return hasChildFile;
}

void DownTool::DownToolBase::GeneratePostProcessLowerZoomTiles(
	std::vector<Tile> currentLevelTiles)
{
	while (!currentLevelTiles.empty() && currentLevelTiles.front().Zoom > 0)
	{
		std::vector<Tile> parentTiles;
		parentTiles.reserve(currentLevelTiles.size() / 4 + 1);
		std::unordered_set<Tile, TileHash, TileEqual> uniqueParents;
		uniqueParents.reserve(currentLevelTiles.size() / 4 + 1);

		for (const Tile& childTile : currentLevelTiles)
		{
			Tile parentTile(childTile.X / 2, childTile.Y / 2, childTile.Zoom - 1);
			if (uniqueParents.insert(parentTile).second)
			{
				parentTiles.push_back(parentTile);
			}
		}

		std::atomic_uint32_t writtenCount = 0;
		std::atomic_uint32_t skippedCount = 0;
		ParallelForIndex(
			parentTiles.size(),
			MaxDownQueue,
			[&](size_t index)
			{
				const int tileSize = static_cast<int>(TileSize);
				const Tile& parentTile = parentTiles[index];
				if (IsPostProcessLowerZoomTileFresh(parentTile))
				{
					skippedCount.fetch_add(1, std::memory_order_relaxed);
					return;
				}

				cv::Mat mergeImage(
					tileSize * 2, tileSize * 2, CV_8UC4, cv::Scalar(0, 0, 0, 0));
				bool hasChildImage = false;

				for (uint32_t childX = 0; childX < 2; childX++)
				{
					for (uint32_t childY = 0; childY < 2; childY++)
					{
						const Tile childTile(
							parentTile.X * 2 + childX,
							parentTile.Y * 2 + childY,
							parentTile.Zoom + 1);
						const cv::Mat childImage =
							ReadBgraTile(GetPostProcessTileFilePath(childTile), TileSize);
						if (childImage.empty())
						{
							continue;
						}

						childImage.copyTo(mergeImage(cv::Rect(
							static_cast<int>(childX) * tileSize,
							static_cast<int>(childY) * tileSize,
							tileSize,
							tileSize)));
						hasChildImage = true;
					}
				}

				if (!hasChildImage)
				{
					return;
				}

				cv::Mat targetImage;
				cv::resize(
					mergeImage,
					targetImage,
					cv::Size(tileSize, tileSize),
					0.,
					0.,
					cv::INTER_AREA);
				if (WriteImage(GetPostProcessTileFilePath(parentTile), targetImage))
				{
					writtenCount.fetch_add(1, std::memory_order_relaxed);
				}
			});

		fmt::print("生成低层级 z{}：写入 {}，跳过 {}，总 {} 张\n",
			parentTiles.front().Zoom,
			writtenCount.load(),
			skippedCount.load(),
			parentTiles.size());
		currentLevelTiles = std::move(parentTiles);
	}
}

cv::Mat DownTool::DownToolBase::ReadPostProcessMaxZoomTile(
	const Tile& targetTile) const
{
	const int tileSize = static_cast<int>(TileSize);
	return ReadPostProcessTileRegion(targetTile, tileSize, tileSize);
}

cv::Mat DownTool::DownToolBase::ReadPostProcessTileRegion(
	const Tile& targetTile,
	int outputWidth,
	int outputHeight) const
{
	return ReadPostProcessTileRegion(targetTile, targetTile.Zoom, outputWidth, outputHeight);
}

cv::Mat DownTool::DownToolBase::ReadPostProcessTileRegion(
	const Tile& targetTile,
	uint32_t sourceZoom,
	int outputWidth,
	int outputHeight) const
{
	if (sourceZoom < targetTile.Zoom)
	{
		return {};
	}

	if (CanCopyPostProcessTileDirectly() && sourceZoom == targetTile.Zoom)
	{
		const std::filesystem::path sourcePath =
			GetPostProcessSourceTileFilePath(targetTile);
		if (!IsValidImageFile(sourcePath) || IsUnavailableTileFile(sourcePath, targetTile))
		{
			return {};
		}

		return ResizeImageToSize(ReadBgraTile(sourcePath, TileSize), outputWidth, outputHeight);
	}

	const int tileSize = static_cast<int>(TileSize);
	if (tileSize <= 0 || outputWidth <= 0 || outputHeight <= 0)
	{
		return {};
	}

	std::vector<TileSample> samples(
		static_cast<size_t>(outputWidth) * static_cast<size_t>(outputHeight));
	std::unordered_set<Tile, TileHash, TileEqual> sourceTiles;
	sourceTiles.reserve(8);

	for (int y = 0; y < outputHeight; y++)
	{
		for (int x = 0; x < outputWidth; x++)
		{
			const double targetPixelX =
				(static_cast<double>(x) + 0.5) * tileSize / outputWidth;
			const double targetPixelY =
				(static_cast<double>(y) + 0.5) * tileSize / outputHeight;
			LonLat targetLonLat;
			if (!TryGetLonLatForTilePixel(
				targetTile, targetPixelX, targetPixelY, targetLonLat))
			{
				continue;
			}

			const LonLat sourceLonLat = ConvertTargetLonLatToSourceLonLat(targetLonLat);
			Tile sourceTile;
			double sourcePixelX = 0.;
			double sourcePixelY = 0.;
			if (!TryGetTilePixelForLonLat(
				sourceLonLat, sourceZoom, sourceTile, sourcePixelX, sourcePixelY))
			{
				continue;
			}

			const size_t sampleIndex = static_cast<size_t>(y * outputWidth + x);
			samples[sampleIndex] = TileSample{
				sourceTile,
				sourcePixelX,
				sourcePixelY,
				true
			};
			sourceTiles.insert(sourceTile);
		}
	}

	if (sourceTiles.empty())
	{
		return {};
	}

	std::unordered_map<Tile, cv::Mat, TileHash, TileEqual> sourceImages;
	sourceImages.reserve(sourceTiles.size());
	for (const Tile& sourceTile : sourceTiles)
	{
		const std::filesystem::path sourcePath =
			GetPostProcessSourceTileFilePath(sourceTile);
		if (!IsValidImageFile(sourcePath) || IsUnavailableTileFile(sourcePath, sourceTile))
		{
			continue;
		}

		cv::Mat sourceImage = ReadBgraTile(sourcePath, TileSize);
		if (!sourceImage.empty())
		{
			sourceImages.emplace(sourceTile, std::move(sourceImage));
		}
	}

	if (sourceImages.empty())
	{
		return {};
	}

	cv::Mat outputImage(outputHeight, outputWidth, CV_8UC4, cv::Scalar(0, 0, 0, 0));
	bool hasPixel = false;
	for (int y = 0; y < outputHeight; y++)
	{
		for (int x = 0; x < outputWidth; x++)
		{
			const TileSample& sample = samples[static_cast<size_t>(y * outputWidth + x)];
			if (!sample.valid)
			{
				continue;
			}

			const auto imageIt = sourceImages.find(sample.sourceTile);
			if (imageIt == sourceImages.end())
			{
				continue;
			}

			outputImage.at<cv::Vec4b>(y, x) =
				SampleBilinear(imageIt->second, sample.pixelX, sample.pixelY);
			hasPixel = true;
		}
	}

	if (!hasPixel)
	{
		return {};
	}

	return outputImage;
}

bool DownTool::DownToolBase::GeneratePostProcessMergedImage(
	bool postProcessTilesAvailable) const
{
	const auto totalStartTime = std::chrono::steady_clock::now();
	const uint32_t targetMaxResolution = PostProcessMergedImageResolution;
	if (targetMaxResolution == 0 ||
		targetMaxResolution > static_cast<uint32_t>(std::numeric_limits<int>::max()))
	{
		fmt::print("合并大图失败：分辨率参数无效。\n");
		return false;
	}

	const uint64_t xCount = static_cast<uint64_t>(MaxTile.X) - MinTile.X + 1;
	const uint64_t yCount = static_cast<uint64_t>(MaxTile.Y) - MinTile.Y + 1;
	if (xCount == 0 || yCount == 0)
	{
		fmt::print("合并大图失败：瓦片范围为空。\n");
		return false;
	}

	const MergedImageOutputSize outputSize = CalculateMergedImageOutputSize(
		xCount,
		yCount,
		TileSize,
		targetMaxResolution);
	if (outputSize.width == 0 || outputSize.height == 0 ||
		outputSize.width > static_cast<uint32_t>(std::numeric_limits<int>::max()) ||
		outputSize.height > static_cast<uint32_t>(std::numeric_limits<int>::max()))
	{
		fmt::print("合并大图失败：输出尺寸计算失败。\n");
		return false;
	}

	const uint32_t sampleZoom = SelectPostProcessMergedImageSampleZoom();
	std::vector<Tile> sampleTiles;
	BuildRequestedTilesAtZoom(sampleZoom, sampleTiles);
	if (sampleTiles.empty())
	{
		fmt::print("合并大图失败：采样层级瓦片范围为空。\n");
		return false;
	}

	fmt::print("合并大图采样层级：z{}，瓦片数 {}，目标最大边 {}，输出分辨率 {}x{}\n",
		sampleZoom,
		sampleTiles.size(),
		targetMaxResolution,
		outputSize.width,
		outputSize.height);

	const std::filesystem::path outputPath = GetPostProcessMergedImagePath();
	const std::filesystem::path metadataPath =
		MakePathWithUtf8Suffix(outputPath, ".meta.json");
	const std::string cacheMetadata = BuildMergedImageCacheMetadata(
		Zoom,
		sampleZoom,
		TileSize,
		targetMaxResolution,
		outputSize.width,
		outputSize.height,
		MinTile.X,
		MinTile.Y,
		MaxTile.X,
		MaxTile.Y,
		CachePath);
	if (IsMergedImageCacheReady(outputPath, metadataPath, cacheMetadata))
	{
		fmt::print("合并大图跳过：输出已存在且参数匹配。\n");
		return true;
	}

	const uint32_t zoomOffset = Zoom - sampleZoom;
	const uint64_t sampleScale = uint64_t{ 1 } << zoomOffset;
	const uint64_t requestMinX = MinTile.X;
	const uint64_t requestMinY = MinTile.Y;
	const uint64_t requestMaxX = static_cast<uint64_t>(MaxTile.X) + 1;
	const uint64_t requestMaxY = static_cast<uint64_t>(MaxTile.Y) + 1;
	const int tileSize = static_cast<int>(TileSize);

	auto isSampleTileFullyCovered = [&](const Tile& tile)
	{
		const uint64_t sampleMinX = static_cast<uint64_t>(tile.X) * sampleScale;
		const uint64_t sampleMinY = static_cast<uint64_t>(tile.Y) * sampleScale;
		const uint64_t sampleMaxX = sampleMinX + sampleScale;
		const uint64_t sampleMaxY = sampleMinY + sampleScale;
		return sampleMinX >= requestMinX &&
			sampleMinY >= requestMinY &&
			sampleMaxX <= requestMaxX &&
			sampleMaxY <= requestMaxY;
	};

	std::vector<Tile> cacheableSampleTiles;
	cacheableSampleTiles.reserve(sampleTiles.size());
	for (const Tile& tile : sampleTiles)
	{
		if (isSampleTileFullyCovered(tile))
		{
			cacheableSampleTiles.push_back(tile);
		}
	}

	fmt::print("大图采样层可缓存瓦片：{} / {}\n",
		cacheableSampleTiles.size(), sampleTiles.size());
	auto phaseStartTime = std::chrono::steady_clock::now();
	if (postProcessTilesAvailable)
	{
		fmt::print("大图采样层后处理跳过：复用已生成的金字塔瓦片。\n");
	}
	else
	{
		phaseStartTime = std::chrono::steady_clock::now();
		if (!GeneratePostProcessTilesAtZoom(
			cacheableSampleTiles, Zoom, "大图采样层后处理"))
		{
			return false;
		}
		fmt::print("大图采样层后处理耗时：{:.2f}s\n", SecondsSince(phaseStartTime));
	}

	const int outputWidth = static_cast<int>(outputSize.width);
	const int outputHeight = static_cast<int>(outputSize.height);
	const double imageMemoryMb =
		static_cast<double>(outputSize.width) * outputSize.height * 4. / 1024. / 1024.;
	fmt::print("合并大图内存估算：{:.1f} MB\n", imageMemoryMb);
	phaseStartTime = std::chrono::steady_clock::now();
	cv::Mat mergedImage(
		outputHeight, outputWidth, CV_8UC4, cv::Scalar(0, 0, 0, 0));
	fmt::print("合并大图内存分配耗时：{:.2f}s\n", SecondsSince(phaseStartTime));

	std::atomic_uint32_t processedCount = 0;
	std::atomic_uint32_t failedCount = 0;

	phaseStartTime = std::chrono::steady_clock::now();
	ParallelForIndex(
		sampleTiles.size(),
		MaxDownQueue,
		[&](size_t index)
		{
			const Tile& tile = sampleTiles[index];
			const uint64_t sampleMinX = static_cast<uint64_t>(tile.X) * sampleScale;
			const uint64_t sampleMinY = static_cast<uint64_t>(tile.Y) * sampleScale;
			const uint64_t sampleMaxX = sampleMinX + sampleScale;
			const uint64_t sampleMaxY = sampleMinY + sampleScale;
			const uint64_t overlapMinX = std::max(sampleMinX, requestMinX);
			const uint64_t overlapMinY = std::max(sampleMinY, requestMinY);
			const uint64_t overlapMaxX = std::min(sampleMaxX, requestMaxX);
			const uint64_t overlapMaxY = std::min(sampleMaxY, requestMaxY);

			if (overlapMaxX <= overlapMinX || overlapMaxY <= overlapMinY)
			{
				const uint32_t currentCount =
					processedCount.fetch_add(1, std::memory_order_relaxed) + 1;
				if (currentCount % 1024 == 0 || currentCount == sampleTiles.size())
				{
					fmt::print("\r大图合并: {}/{}",
						currentCount, sampleTiles.size());
				}
				return;
			}

			const int x0 = static_cast<int>(
				((overlapMinX - requestMinX) * outputSize.width) / xCount);
			const int x1 = static_cast<int>(
				((overlapMaxX - requestMinX) * outputSize.width) / xCount);
			const int y0 = static_cast<int>(
				((overlapMinY - requestMinY) * outputSize.height) / yCount);
			const int y1 = static_cast<int>(
				((overlapMaxY - requestMinY) * outputSize.height) / yCount);

			if (x1 <= x0 || y1 <= y0)
			{
				const uint32_t currentCount =
					processedCount.fetch_add(1, std::memory_order_relaxed) + 1;
				if (currentCount % 1024 == 0 || currentCount == sampleTiles.size())
				{
					fmt::print("\r大图合并: {}/{}",
						currentCount, sampleTiles.size());
				}
				return;
			}

			const bool useCachedTile = isSampleTileFullyCovered(tile);
			const cv::Mat tileImage = useCachedTile ?
				ReadBgraTile(GetPostProcessTileFilePath(tile), TileSize) :
				ReadPostProcessTileRegion(tile, Zoom, tileSize, tileSize);
			if (tileImage.empty())
			{
				failedCount.fetch_add(1, std::memory_order_relaxed);
				const uint32_t currentCount =
					processedCount.fetch_add(1, std::memory_order_relaxed) + 1;
				if (currentCount % 1024 == 0 || currentCount == sampleTiles.size())
				{
					fmt::print("\r大图合并: {}/{}",
						currentCount, sampleTiles.size());
				}
				return;
			}

			const double cropX0 =
				static_cast<double>(overlapMinX - sampleMinX) *
				tileSize / static_cast<double>(sampleScale);
			const double cropY0 =
				static_cast<double>(overlapMinY - sampleMinY) *
				tileSize / static_cast<double>(sampleScale);
			const double cropX1 =
				static_cast<double>(overlapMaxX - sampleMinX) *
				tileSize / static_cast<double>(sampleScale);
			const double cropY1 =
				static_cast<double>(overlapMaxY - sampleMinY) *
				tileSize / static_cast<double>(sampleScale);
			const int cropLeft =
				std::clamp(static_cast<int>(std::floor(cropX0)), 0, tileSize - 1);
			const int cropTop =
				std::clamp(static_cast<int>(std::floor(cropY0)), 0, tileSize - 1);
			const int cropRight =
				std::clamp(static_cast<int>(std::ceil(cropX1)), cropLeft + 1, tileSize);
			const int cropBottom =
				std::clamp(static_cast<int>(std::ceil(cropY1)), cropTop + 1, tileSize);
			const cv::Mat croppedTile = tileImage(cv::Rect(
				cropLeft,
				cropTop,
				cropRight - cropLeft,
				cropBottom - cropTop));
			cv::Mat targetRoi = mergedImage(cv::Rect(x0, y0, x1 - x0, y1 - y0));
			const cv::InterpolationFlags interpolation =
				(croppedTile.cols > targetRoi.cols || croppedTile.rows > targetRoi.rows) ?
				cv::INTER_AREA : cv::INTER_LINEAR;
			cv::resize(croppedTile, targetRoi, targetRoi.size(), 0., 0., interpolation);

			const uint32_t currentCount =
				processedCount.fetch_add(1, std::memory_order_relaxed) + 1;
			if (currentCount % 1024 == 0 || currentCount == sampleTiles.size())
			{
				fmt::print("\r大图合并: {}/{}",
					currentCount, sampleTiles.size());
			}
		});

	fmt::print("\r大图合并: {}/{}，失败 {}\n",
		sampleTiles.size(), sampleTiles.size(), failedCount.load());
	fmt::print("大图像素合并耗时：{:.2f}s\n", SecondsSince(phaseStartTime));
	if (failedCount.load() != 0)
	{
		return false;
	}

	phaseStartTime = std::chrono::steady_clock::now();
	const bool writeSuccess = WriteImage(outputPath, mergedImage);
	fmt::print("合并大图写入耗时：{:.2f}s\n", SecondsSince(phaseStartTime));
	if (writeSuccess)
	{
		if (!WriteTextFile(metadataPath, cacheMetadata))
		{
			fmt::print("合并大图警告：缓存元数据写入失败。\n");
		}
		fmt::print("合并大图输出：{}\n", PathToUtf8String(outputPath));
	}
	fmt::print("合并大图函数总耗时：{:.2f}s\n", SecondsSince(totalStartTime));
	return writeSuccess;
}

bool DownTool::DownToolBase::WritePostProcessTileFromSourceZoom(
	const Tile& targetTile,
	const std::filesystem::path& outputPath,
	uint32_t sourceZoom) const
{
	if (CanCopyPostProcessTileDirectly() && sourceZoom == targetTile.Zoom)
	{
		const std::filesystem::path sourcePath =
			GetPostProcessSourceTileFilePath(targetTile);
		if (!IsValidImageFile(sourcePath) || IsUnavailableTileFile(sourcePath, targetTile))
		{
			return false;
		}

		return CopyFileReplacing(sourcePath, outputPath);
	}

	const int tileSize = static_cast<int>(TileSize);
	return WriteImage(
		outputPath,
		ReadPostProcessTileRegion(targetTile, sourceZoom, tileSize, tileSize));
}

bool DownTool::DownToolBase::WritePostProcessMaxZoomTile(
	const Tile& targetTile,
	const std::filesystem::path& outputPath) const
{
	return WritePostProcessTileFromSourceZoom(targetTile, outputPath, targetTile.Zoom);
}

bool DownTool::DownToolBase::WritePostProcessMetadata() const
{
	const PostProcessMetadata metadata{
		PostProcessBounds{
			MinLonLat.lon,
			MaxLonLat.lon,
			MinLonLat.lat,
			MaxLonLat.lat },
		Zoom,
		TileSize
	};
	const std::filesystem::path outputRoot = GetPostProcessRootPath();

	const std::string json = BuildMetaJson(metadata);
	const std::string xml = BuildTileMapResourceXml(metadata);
	if (json.empty() || xml.empty())
	{
		return false;
	}

	const bool jsonSuccess = WriteTextFile(outputRoot / "meta.json", json);
	const bool xmlSuccess = WriteTextFile(outputRoot / "tilemapresource.xml", xml);
	return jsonSuccess && xmlSuccess;
}

std::string DownTool::DownToolBase::MakeUri(const Tile& tile)
{
	const uint32_t xValue = tile.X;
	const uint32_t yValue = tile.Y;
	const uint32_t zValue = tile.Zoom;
	const int32_t sValue = ServerRange.GetNextServerIndex();
	auto x = fmt::arg("x", xValue);
	auto y = fmt::arg("y", yValue);
	auto z = fmt::arg("z", zValue);
	auto s = fmt::arg("s", sValue);
	fmt::format_args args = fmt::make_format_args(x, y, z, s);
	return fmt::vformat(UrlTemplate, args);
}

void DownTool::DownToolBase::CalculateDownRange()
{
	GeographicLib::Math::real dummy;
	auto& geo = GeographicLib::Geodesic::WGS84();
	geo.Direct(GetCenterLonLat().lat, GetCenterLonLat().lon, 0., GetRadius(), MaxLonLat.lat, dummy);
	geo.Direct(GetCenterLonLat().lat, GetCenterLonLat().lon, 180., GetRadius(), MinLonLat.lat, dummy);
	geo.Direct(GetCenterLonLat().lat, GetCenterLonLat().lon, 90., GetRadius(), dummy, MaxLonLat.lon);
	geo.Direct(GetCenterLonLat().lat, GetCenterLonLat().lon, 270., GetRadius(), dummy, MinLonLat.lon);

	// 处理经度越界（GeographicLib 通常返回 [-180, 180]）
	if (MinLonLat.lon > MaxLonLat.lon) {
		// 跨越了 ±180° 经线，此时 bounding box 包围极点或环绕地球
		// 简单处理：设为全球（根据需求调整）
		MaxLonLat.lon = 180.0;
		MinLonLat.lon = -180.0;
	}
}
