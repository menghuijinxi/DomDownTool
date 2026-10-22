#pragma once

#include <fstream>
#include <memory>
#include <string>
#include <type_traits>

#include "shapefil.h"

#include "PostProcessTestSupport.h"

namespace shapefile_test
{
constexpr const char* kWgs84Prj =
    R"(GEOGCS["GCS_WGS_1984",DATUM["D_WGS_1984",SPHEROID["WGS_1984",6378137,298.257223563]],PRIMEM["Greenwich",0],UNIT["Degree",0.0174532925199433]])";

inline void WriteText(const std::filesystem::path& path, const std::string& text)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text;
}

inline bool WriteFixture(const std::filesystem::path& path, int shapeType = SHPT_POLYGON)
{
    using Shapes = std::unique_ptr<std::remove_pointer_t<SHPHandle>, decltype(&SHPClose)>;
    using Attributes = std::unique_ptr<std::remove_pointer_t<DBFHandle>, decltype(&DBFClose)>;
    using Object = std::unique_ptr<SHPObject, decltype(&SHPDestroyObject)>;
    std::filesystem::create_directories(path.parent_path());
    SAHooks hooks{};
#ifdef SHPAPI_UTF8_HOOKS
    SASetupUtf8Hooks(&hooks);
#else
    SASetupDefaultHooks(&hooks);
#endif
    const auto textPath = postprocess_test::PathToUtf8String(path);
    Shapes shapes(SHPCreateLL(textPath.c_str(), shapeType, &hooks), &SHPClose);
    auto cpg = path;
    cpg.replace_extension(".cpg");
    WriteText(cpg, "UTF-8");
    Attributes attributes(DBFCreateLL(textPath.c_str(), "UTF-8", &hooks), &DBFClose);
    if (!shapes || !attributes || DBFAddField(attributes.get(), "height_m", FTDouble, 20, 3) < 0 ||
        DBFAddField(attributes.get(), "name", FTString, 120, 0) < 0 ||
        DBFAddField(attributes.get(), "owner", FTString, 120, 0) < 0 ||
        DBFAddField(attributes.get(), "uid", FTString, 20, 0) < 0)
    {
        return false;
    }
    for (int record = 0; record < 2; ++record)
    {
        const double offset = record * 0.02;
        const std::vector<double> x = {116.42 + offset,   116.422 + offset,  116.422 + offset,
                                       116.42 + offset,   116.42 + offset,   116.4205 + offset,
                                       116.4205 + offset, 116.4215 + offset, 116.4215 + offset,
                                       116.4205 + offset};
        const std::vector<double> y = {39.89,   39.89,   39.892,  39.892,  39.89,
                                       39.8905, 39.8915, 39.8915, 39.8905, 39.8905};
        const std::vector<int> starts = {0, 5};
        const int vertices = shapeType == SHPT_POINT ? 1 : static_cast<int>(x.size());
        const bool multipart = shapeType == SHPT_POLYGON || shapeType == SHPT_ARC;
        Object object(SHPCreateObject(shapeType, -1, multipart ? 2 : 0,
                                      multipart ? starts.data() : nullptr, nullptr, vertices,
                                      x.data(), y.data(), nullptr, nullptr),
                      &SHPDestroyObject);
        if (!object || SHPWriteObject(shapes.get(), -1, object.get()) != record ||
            !DBFWriteDoubleAttribute(attributes.get(), record, 0, 12.345 + record) ||
            !DBFWriteStringAttribute(attributes.get(), record, 1, "中文建筑") ||
            !DBFWriteStringAttribute(attributes.get(), record, 2, "业主甲") ||
            !DBFWriteStringAttribute(attributes.get(), record, 3, "18446744073709551615"))
        {
            return false;
        }
    }
    auto prj = path;
    prj.replace_extension(".prj");
    WriteText(prj, kWgs84Prj);
    return true;
}
} // namespace shapefile_test
