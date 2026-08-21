#include "preview/Dx12FbxRenderer.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <utility>

#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include "include/codec/SkCodec.h"
#include "include/core/SkBitmap.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkData.h"
#include "include/core/SkImage.h"
#include "include/core/SkImageInfo.h"
#include "include/gpu/GpuTypes.h"
#include "include/gpu/ganesh/GrBackendSurface.h"
#include "include/gpu/ganesh/GrDirectContext.h"
#include "include/gpu/ganesh/GrRecordingContext.h"
#include "include/gpu/ganesh/SkSurfaceGanesh.h"
#include "include/gpu/ganesh/d3d/GrD3DBackendSurface.h"
#include "include/gpu/ganesh/d3d/GrD3DTypes.h"
#include "src/gpu/ganesh/GrDirectContextPriv.h"
#include "src/gpu/ganesh/d3d/GrD3DGpu.h"

#include "preview/SkiaImageCodecs.h"

using Microsoft::WRL::ComPtr;

namespace
{
constexpr DXGI_FORMAT kColorFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_D32_FLOAT;
constexpr std::size_t kMaximumVertices = 250000;
constexpr std::size_t kMaximumTriangles = 500000;
constexpr std::size_t kMaximumTextures = 1024;
constexpr int kMaximumTextureDimension = 4096;
constexpr UINT kFrameCount = 3;
constexpr UINT kRootConstantCount = 20;
constexpr UINT kTextureDescriptorCount = static_cast<UINT>(kMaximumTextures);

struct Bounds
{
    std::array<float, 3> minimum{
        std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max()};
    std::array<float, 3> maximum{
        std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest()};
};

struct GpuVertex
{
    float x = 0.F;
    float y = 0.F;
    float z = 0.F;
    float u = 0.F;
    float v = 0.F;
};

struct CpuBatch
{
    std::vector<GpuVertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<uint32_t> edgeIndices;
    uint32_t textureIndex = std::numeric_limits<uint32_t>::max();
};

struct CpuTexture
{
    std::filesystem::path path;
    int width = 0;
    int height = 0;
    std::vector<uint8_t> bc1;
};

struct UploadLifetime
{
    std::vector<ComPtr<ID3D12Resource>> uploads;
    std::vector<ComPtr<ID3D12Resource>> resources;
};

struct Frame
{
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commandList;
    UINT64 fenceValue = 0;
    std::shared_ptr<UploadLifetime> retainedUpload;
};

struct GpuBatch
{
    const CpuBatch* source = nullptr;
    ComPtr<ID3D12Resource> vertexBuffer;
    ComPtr<ID3D12Resource> indexBuffer;
    ComPtr<ID3D12Resource> edgeBuffer;
    UINT indexCount = 0;
    UINT edgeIndexCount = 0;
};

struct RootConstants
{
    float transform[16]{};
    uint32_t textureIndex = std::numeric_limits<uint32_t>::max();
    uint32_t flags = 0;
    float color[2]{0.28F, 0.62F};
};

static_assert(sizeof(RootConstants) == kRootConstantCount * sizeof(uint32_t));

std::string pathToUtf8(const std::filesystem::path& path)
{
    const std::u8string value = path.u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

void includePoint(Bounds& bounds, const std::array<float, 3>& point)
{
    for (std::size_t index = 0; index < point.size(); ++index)
    {
        bounds.minimum[index] = std::min(bounds.minimum[index], point[index]);
        bounds.maximum[index] = std::max(bounds.maximum[index], point[index]);
    }
}

aiMatrix4x4 nodeTransform(const aiNode* node, const aiMatrix4x4& parent)
{
    return parent * node->mTransformation;
}

sk_sp<SkImage> loadImage(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open())
    {
        return nullptr;
    }
    std::vector<uint8_t> encoded;
    encoded.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    if (encoded.empty())
    {
        return nullptr;
    }
    std::unique_ptr<SkCodec> codec = SkCodec::MakeFromData(
        SkData::MakeWithCopy(encoded.data(), encoded.size()));
    if (!codec)
    {
        return nullptr;
    }
    const SkISize sourceSize = codec->dimensions();
    if (sourceSize.width() <= 0 || sourceSize.height() <= 0)
    {
        return nullptr;
    }
    const int maximumDimension = std::max(sourceSize.width(), sourceSize.height());
    const float scale = maximumDimension > kMaximumTextureDimension
        ? static_cast<float>(kMaximumTextureDimension) / static_cast<float>(maximumDimension)
        : 1.F;
    const SkISize size = codec->getScaledDimensions(scale);
    const SkImageInfo info = SkImageInfo::MakeN32Premul(size.width(), size.height());
    SkBitmap bitmap;
    if (!bitmap.tryAllocPixels(info) ||
        codec->getPixels(info, bitmap.getPixels(), bitmap.rowBytes()) != SkCodec::kSuccess)
    {
        return nullptr;
    }
    bitmap.setImmutable();
    return SkImages::RasterFromBitmap(bitmap);
}

uint32_t color565(uint8_t red, uint8_t green, uint8_t blue)
{
    return (static_cast<uint32_t>(red) * 31U / 255U << 11U) |
        (static_cast<uint32_t>(green) * 63U / 255U << 5U) |
        (static_cast<uint32_t>(blue) * 31U / 255U);
}

std::array<uint8_t, 3> color565ToRgb(uint32_t color)
{
    return {
        static_cast<uint8_t>(((color >> 11U) & 31U) * 255U / 31U),
        static_cast<uint8_t>(((color >> 5U) & 63U) * 255U / 63U),
        static_cast<uint8_t>((color & 31U) * 255U / 31U)};
}

int colorDistanceSquared(const std::array<uint8_t, 3>& lhs,
                         const std::array<uint8_t, 3>& rhs)
{
    const int red = static_cast<int>(lhs[0]) - rhs[0];
    const int green = static_cast<int>(lhs[1]) - rhs[1];
    const int blue = static_cast<int>(lhs[2]) - rhs[2];
    return red * red + green * green + blue * blue;
}

std::array<std::array<uint8_t, 3>, 4> makeBc1Palette(uint16_t endpoint0,
                                                     uint16_t endpoint1)
{
    std::array<std::array<uint8_t, 3>, 4> palette{};
    palette[0] = color565ToRgb(endpoint0);
    palette[1] = color565ToRgb(endpoint1);
    for (std::size_t channel = 0; channel < palette[0].size(); ++channel)
    {
        palette[2][channel] = static_cast<uint8_t>(
            (2U * palette[0][channel] + palette[1][channel]) / 3U);
        palette[3][channel] = static_cast<uint8_t>(
            (palette[0][channel] + 2U * palette[1][channel]) / 3U);
    }
    return palette;
}

std::pair<uint16_t, uint16_t> chooseBc1Endpoints(
    const std::array<std::array<uint8_t, 3>, 16>& pixels)
{
    std::size_t first = 0;
    std::size_t second = 0;
    int maximumDistance = -1;
    for (std::size_t lhs = 0; lhs < pixels.size(); ++lhs)
    {
        for (std::size_t rhs = lhs + 1; rhs < pixels.size(); ++rhs)
        {
            const int distance = colorDistanceSquared(pixels[lhs], pixels[rhs]);
            if (distance > maximumDistance)
            {
                maximumDistance = distance;
                first = lhs;
                second = rhs;
            }
        }
    }
    uint16_t endpoint0 = static_cast<uint16_t>(
        color565(pixels[first][0], pixels[first][1], pixels[first][2]));
    uint16_t endpoint1 = static_cast<uint16_t>(
        color565(pixels[second][0], pixels[second][1], pixels[second][2]));
    if (endpoint0 == endpoint1)
    {
        if (endpoint0 < std::numeric_limits<uint16_t>::max())
        {
            ++endpoint0;
        }
        else
        {
            --endpoint1;
        }
    }
    if (endpoint0 < endpoint1)
    {
        std::swap(endpoint0, endpoint1);
    }
    return {endpoint0, endpoint1};
}

std::vector<uint8_t> encodeBc1(const SkImage& image)
{
    SkBitmap bitmap;
    const SkImageInfo info = SkImageInfo::MakeN32Premul(image.width(), image.height());
    if (!bitmap.tryAllocPixels(info) || !image.readPixels(info, bitmap.getPixels(), bitmap.rowBytes(), 0, 0))
    {
        return {};
    }
    const int blockWidth = (image.width() + 3) / 4;
    const int blockHeight = (image.height() + 3) / 4;
    std::vector<uint8_t> output(static_cast<std::size_t>(blockWidth) * blockHeight * 8U);
    for (int blockY = 0; blockY < blockHeight; ++blockY)
    {
        for (int blockX = 0; blockX < blockWidth; ++blockX)
        {
            std::array<std::array<uint8_t, 3>, 16> pixels{};
            for (int y = 0; y < 4; ++y)
            {
                for (int x = 0; x < 4; ++x)
                {
                    const int px = std::min(blockX * 4 + x, image.width() - 1);
                    const int py = std::min(blockY * 4 + y, image.height() - 1);
                    const SkColor color = bitmap.getColor(px, py);
                    pixels[static_cast<std::size_t>(y * 4 + x)] = {
                        SkColorGetR(color), SkColorGetG(color), SkColorGetB(color)};
                }
            }
            const auto [endpoint0, endpoint1] = chooseBc1Endpoints(pixels);
            const auto palette = makeBc1Palette(endpoint0, endpoint1);
            uint32_t indices = 0;
            for (std::size_t index = 0; index < pixels.size(); ++index)
            {
                uint32_t nearest = 0;
                int nearestDistance = colorDistanceSquared(pixels[index], palette[0]);
                for (uint32_t paletteIndex = 1; paletteIndex < palette.size(); ++paletteIndex)
                {
                    const int distance = colorDistanceSquared(
                        pixels[index], palette[paletteIndex]);
                    if (distance < nearestDistance)
                    {
                        nearest = paletteIndex;
                        nearestDistance = distance;
                    }
                }
                indices |= nearest << (index * 2U);
            }
            const std::size_t offset =
                (static_cast<std::size_t>(blockY) * blockWidth + blockX) * 8U;
            std::memcpy(output.data() + offset, &endpoint0, sizeof(endpoint0));
            std::memcpy(output.data() + offset + sizeof(endpoint0), &endpoint1,
                        sizeof(endpoint1));
            std::memcpy(output.data() + offset + 4, &indices, sizeof(indices));
        }
    }
    return output;
}

ComPtr<ID3DBlob> compileShader(const char* source, const char* entryPoint, const char* target)
{
    ComPtr<ID3DBlob> code;
    ComPtr<ID3DBlob> errors;
    if (FAILED(D3DCompile(source, std::strlen(source), nullptr, nullptr, nullptr, entryPoint,
                          target, D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &errors)))
    {
        return nullptr;
    }
    return code;
}

const char* kVertexShader = R"(
cbuffer Constants : register(b0) {
    float4x4 transform;
    uint textureIndex;
    uint flags;
    float2 color;
};
struct Input { float3 position : POSITION; float2 uv : TEXCOORD0; };
struct Output { float4 position : SV_POSITION; float2 uv : TEXCOORD0; };
Output main(Input input) {
    Output output;
    output.position = mul(float4(input.position, 1.0), transform);
    output.uv = float2(input.uv.x, 1.0 - input.uv.y);
    return output;
}
)";

const char* kTexturedPixelShader = R"(
Texture2D textures[1024] : register(t0);
SamplerState samplerState : register(s0);
cbuffer Constants : register(b0) {
    float4x4 transform;
    uint textureIndex;
    uint flags;
    float2 color;
};
struct Input { float4 position : SV_POSITION; float2 uv : TEXCOORD0; };
float4 main(Input input) : SV_TARGET {
    if (textureIndex == 0xffffffff) return float4(color.x, color.y, 0.42, 1.0);
    return textures[textureIndex].Sample(samplerState, input.uv);
}
)";

const char* kWirePixelShader = R"(
cbuffer Constants : register(b0) {
    float4x4 transform;
    uint textureIndex;
    uint flags;
    float2 color;
};
float4 main() : SV_TARGET { return float4(0.55, 0.90, 0.78, 0.72); }
)";

struct Dx12FbxState
{
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> fence;
    HANDLE fenceEvent = nullptr;
    UINT64 fenceValue = 0;
    std::array<Frame, kFrameCount> frames;
    UINT frameIndex = 0;
    ComPtr<ID3D12RootSignature> rootSignature;
    ComPtr<ID3D12PipelineState> texturedPipeline;
    ComPtr<ID3D12PipelineState> wirePipeline;
    ComPtr<ID3D12DescriptorHeap> srvHeap;
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    ComPtr<ID3D12DescriptorHeap> dsvHeap;
    UINT descriptorSize = 0;
    ComPtr<ID3D12Resource> depth;
    UINT depthWidth = 0;
    UINT depthHeight = 0;
    std::vector<ComPtr<ID3D12Resource>> textures;
    std::vector<GpuBatch> batches;
    UINT64 loadedRevision = 0;

    ~Dx12FbxState()
    {
        waitForFrames();
        if (fenceEvent)
        {
            CloseHandle(fenceEvent);
        }
    }

    bool waitForFence(UINT64 value)
    {
        if (value == 0)
        {
            return true;
        }
        if (!fence || !fenceEvent)
        {
            return false;
        }
        if (fence->GetCompletedValue() >= value)
        {
            return true;
        }
        return SUCCEEDED(fence->SetEventOnCompletion(value, fenceEvent)) &&
            WaitForSingleObject(fenceEvent, INFINITE) == WAIT_OBJECT_0;
    }

    bool waitForFrames()
    {
        bool success = true;
        for (Frame& frame : frames)
        {
            success = waitForFence(frame.fenceValue) && success;
            frame.fenceValue = 0;
            frame.retainedUpload.reset();
        }
        return success;
    }

    void reset()
    {
        waitForFrames();
        if (fenceEvent)
        {
            CloseHandle(fenceEvent);
            fenceEvent = nullptr;
        }
        for (Frame& frame : frames)
        {
            frame.commandList.Reset();
            frame.allocator.Reset();
            frame.fenceValue = 0;
            frame.retainedUpload.reset();
        }
        textures.clear();
        batches.clear();
        depth.Reset();
        dsvHeap.Reset();
        srvHeap.Reset();
        rtvHeap.Reset();
        wirePipeline.Reset();
        texturedPipeline.Reset();
        rootSignature.Reset();
        fence.Reset();
        queue.Reset();
        device.Reset();
        fenceValue = 0;
        frameIndex = 0;
        loadedRevision = 0;
        depthWidth = 0;
        depthHeight = 0;
    }
};

bool initializeState(Dx12FbxState& state, ID3D12Resource* target, ID3D12CommandQueue* queue)
{
    ComPtr<ID3D12Device> device;
    if (!target || !queue || FAILED(target->GetDevice(IID_PPV_ARGS(&device))))
    {
        return false;
    }
    state.reset();
    state.device = std::move(device);
    state.queue = queue;
    if (FAILED(state.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&state.fence))))
    {
        state.reset();
        return false;
    }
    state.fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!state.fenceEvent)
    {
        state.reset();
        return false;
    }
    for (Frame& frame : state.frames)
    {
        if (FAILED(state.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                         IID_PPV_ARGS(&frame.allocator))) ||
            FAILED(state.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                    frame.allocator.Get(), nullptr,
                                                    IID_PPV_ARGS(&frame.commandList))) ||
            FAILED(frame.commandList->Close()))
        {
            state.reset();
            return false;
        }
    }
    const D3D12_DESCRIPTOR_HEAP_DESC srvDescription{
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kTextureDescriptorCount,
        D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    const D3D12_DESCRIPTOR_HEAP_DESC rtvDescription{
        D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    const D3D12_DESCRIPTOR_HEAP_DESC dsvDescription{
        D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    if (FAILED(state.device->CreateDescriptorHeap(&srvDescription,
                                                  IID_PPV_ARGS(&state.srvHeap))) ||
        FAILED(state.device->CreateDescriptorHeap(&dsvDescription,
                                                  IID_PPV_ARGS(&state.dsvHeap))) ||
        FAILED(state.device->CreateDescriptorHeap(&rtvDescription,
                                                  IID_PPV_ARGS(&state.rtvHeap))))
    {
        state.reset();
        return false;
    }
    state.descriptorSize = state.device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_ROOT_PARAMETER rootParameter{};
    rootParameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rootParameter.Constants.Num32BitValues = kRootConstantCount;
    rootParameter.Constants.ShaderRegister = 0;
    rootParameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_DESCRIPTOR_RANGE textureRange{
        D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kTextureDescriptorCount, 0, 0,
        D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND};
    D3D12_ROOT_PARAMETER descriptorParameter{};
    descriptorParameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    descriptorParameter.DescriptorTable.NumDescriptorRanges = 1;
    descriptorParameter.DescriptorTable.pDescriptorRanges = &textureRange;
    descriptorParameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    const D3D12_ROOT_PARAMETER parameters[] = {rootParameter, descriptorParameter};
    const D3D12_ROOT_SIGNATURE_DESC rootDescription{
        static_cast<UINT>(std::size(parameters)), parameters, 1, &sampler,
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
    ComPtr<ID3DBlob> rootBlob;
    ComPtr<ID3DBlob> rootErrors;
    if (FAILED(D3D12SerializeRootSignature(&rootDescription, D3D_ROOT_SIGNATURE_VERSION_1,
                                           &rootBlob, &rootErrors)) ||
        FAILED(state.device->CreateRootSignature(0, rootBlob->GetBufferPointer(),
                                                  rootBlob->GetBufferSize(),
                                                  IID_PPV_ARGS(&state.rootSignature))))
    {
        state.reset();
        return false;
    }
    const ComPtr<ID3DBlob> vertexShader = compileShader(kVertexShader, "main", "vs_5_0");
    const ComPtr<ID3DBlob> texturedShader =
        compileShader(kTexturedPixelShader, "main", "ps_5_0");
    const ComPtr<ID3DBlob> wireShader = compileShader(kWirePixelShader, "main", "ps_5_0");
    if (!vertexShader || !texturedShader || !wireShader)
    {
        state.reset();
        return false;
    }
    const D3D12_INPUT_ELEMENT_DESC input[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
    const auto makePipeline = [&](ID3DBlob* pixelShader, D3D12_PRIMITIVE_TOPOLOGY_TYPE topology,
                                  bool textured, ComPtr<ID3D12PipelineState>& result)
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
        description.pRootSignature = state.rootSignature.Get();
        description.VS = {vertexShader->GetBufferPointer(), vertexShader->GetBufferSize()};
        description.PS = {pixelShader->GetBufferPointer(), pixelShader->GetBufferSize()};
        description.InputLayout = {input, static_cast<UINT>(std::size(input))};
        description.PrimitiveTopologyType = topology;
        description.NumRenderTargets = 1;
        description.RTVFormats[0] = kColorFormat;
        description.DSVFormat = kDepthFormat;
        description.SampleMask = D3D12_DEFAULT_SAMPLE_MASK;
        description.SampleDesc = {1, 0};
        description.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE, FALSE,
                                       D3D12_DEFAULT_DEPTH_BIAS, D3D12_DEFAULT_DEPTH_BIAS_CLAMP,
                                       D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS, TRUE, FALSE, FALSE,
                                       0, D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF};
        description.DepthStencilState.DepthEnable = TRUE;
        description.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        description.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        description.DepthStencilState.StencilEnable = FALSE;
        description.BlendState.RenderTarget[0].BlendEnable = !textured;
        description.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
        description.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        description.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
        description.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
        description.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        description.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
        description.BlendState.RenderTarget[0].RenderTargetWriteMask =
            D3D12_COLOR_WRITE_ENABLE_ALL;
        return SUCCEEDED(state.device->CreateGraphicsPipelineState(
            &description, IID_PPV_ARGS(&result)));
    };
    if (!makePipeline(texturedShader.Get(), D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, true,
                      state.texturedPipeline) ||
        !makePipeline(wireShader.Get(), D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE, false,
                      state.wirePipeline))
    {
        state.reset();
        return false;
    }
    return true;
}

bool ensureDepth(Dx12FbxState& state, UINT width, UINT height)
{
    if (state.depth && state.depthWidth == width && state.depthHeight == height)
    {
        return true;
    }
    state.waitForFrames();
    state.depth.Reset();
    const D3D12_HEAP_PROPERTIES properties{
        D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
        D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
    const D3D12_RESOURCE_DESC description{
        D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, width, height, 1, 1, kDepthFormat,
        {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL};
    const D3D12_CLEAR_VALUE clearValue{kDepthFormat, {1.F, 0}};
    if (FAILED(state.device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE,
                                                     &description, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                                     &clearValue, IID_PPV_ARGS(&state.depth))))
    {
        return false;
    }
    D3D12_DEPTH_STENCIL_VIEW_DESC view{};
    view.Format = kDepthFormat;
    view.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    state.device->CreateDepthStencilView(state.depth.Get(), &view,
                                         state.dsvHeap->GetCPUDescriptorHandleForHeapStart());
    state.depthWidth = width;
    state.depthHeight = height;
    return true;
}

bool createBuffer(ID3D12Device* device, ID3D12GraphicsCommandList* commandList,
                  const void* data, UINT64 byteSize, ComPtr<ID3D12Resource>& resource,
                  ComPtr<ID3D12Resource>& upload)
{
    const D3D12_HEAP_PROPERTIES uploadProperties{
        D3D12_HEAP_TYPE_UPLOAD, D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
        D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
    const D3D12_RESOURCE_DESC description{
        D3D12_RESOURCE_DIMENSION_BUFFER, 0, byteSize, 1, 1, 1, DXGI_FORMAT_UNKNOWN,
        {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE};
    if (FAILED(device->CreateCommittedResource(&uploadProperties, D3D12_HEAP_FLAG_NONE,
                                               &description, D3D12_RESOURCE_STATE_GENERIC_READ,
                                               nullptr, IID_PPV_ARGS(&upload))))
    {
        return false;
    }
    void* mapped = nullptr;
    const D3D12_RANGE range{0, 0};
    if (FAILED(upload->Map(0, &range, &mapped)) || !mapped)
    {
        return false;
    }
    std::memcpy(mapped, data, static_cast<std::size_t>(byteSize));
    upload->Unmap(0, nullptr);
    const D3D12_HEAP_PROPERTIES defaultProperties{
        D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
        D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
    if (FAILED(device->CreateCommittedResource(&defaultProperties, D3D12_HEAP_FLAG_NONE,
                                               &description, D3D12_RESOURCE_STATE_COPY_DEST,
                                               nullptr, IID_PPV_ARGS(&resource))))
    {
        resource = upload;
        upload.Reset();
        return true;
    }
    commandList->CopyBufferRegion(resource.Get(), 0, upload.Get(), 0, byteSize);
    const D3D12_RESOURCE_BARRIER barrier{
        D3D12_RESOURCE_BARRIER_TYPE_TRANSITION, D3D12_RESOURCE_BARRIER_FLAG_NONE,
        {resource.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
         D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_GENERIC_READ}};
    commandList->ResourceBarrier(1, &barrier);
    return true;
}

bool uploadTexture(ID3D12Device* device, ID3D12GraphicsCommandList* commandList,
                   const CpuTexture& source, ComPtr<ID3D12Resource>& resource,
                   ComPtr<ID3D12Resource>& upload)
{
    const UINT width = static_cast<UINT>((source.width + 3) / 4);
    const UINT height = static_cast<UINT>((source.height + 3) / 4);
    const UINT rowPitch = width * 8U;
    const D3D12_RESOURCE_DESC textureDescription{
        D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, static_cast<UINT64>(source.width),
        static_cast<UINT>(source.height), 1, 1, DXGI_FORMAT_BC1_UNORM, {1, 0},
        D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_NONE};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rowCount = 0;
    UINT64 rowSize = 0;
    UINT64 requiredSize = 0;
    device->GetCopyableFootprints(&textureDescription, 0, 1, 0, &footprint, &rowCount,
                                  &rowSize, &requiredSize);
    const UINT64 uploadSize = requiredSize;
    const D3D12_HEAP_PROPERTIES uploadProperties{
        D3D12_HEAP_TYPE_UPLOAD, D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
        D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
    const D3D12_RESOURCE_DESC uploadDescription{
        D3D12_RESOURCE_DIMENSION_BUFFER, 0, uploadSize, 1, 1, 1, DXGI_FORMAT_UNKNOWN,
        {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE};
    if (FAILED(device->CreateCommittedResource(&uploadProperties, D3D12_HEAP_FLAG_NONE,
                                               &uploadDescription, D3D12_RESOURCE_STATE_GENERIC_READ,
                                               nullptr, IID_PPV_ARGS(&upload))))
    {
        return false;
    }
    void* mapped = nullptr;
    const D3D12_RANGE range{0, 0};
    if (FAILED(upload->Map(0, &range, &mapped)) || !mapped)
    {
        return false;
    }
    for (UINT row = 0; row < rowCount; ++row)
    {
        std::memcpy(static_cast<uint8_t*>(mapped) + row * footprint.Footprint.RowPitch,
                    source.bc1.data() + static_cast<std::size_t>(row) * rowPitch,
                    rowPitch);
    }
    upload->Unmap(0, nullptr);
    const D3D12_HEAP_PROPERTIES defaultProperties{
        D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
        D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
    if (FAILED(device->CreateCommittedResource(&defaultProperties, D3D12_HEAP_FLAG_NONE,
                                               &textureDescription, D3D12_RESOURCE_STATE_COPY_DEST,
                                               nullptr, IID_PPV_ARGS(&resource))))
    {
        return false;
    }
    D3D12_TEXTURE_COPY_LOCATION destination{resource.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    destination.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION sourceLocation{upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    sourceLocation.PlacedFootprint = footprint;
    commandList->CopyTextureRegion(&destination, 0, 0, 0, &sourceLocation, nullptr);
    const D3D12_RESOURCE_BARRIER barrier{
        D3D12_RESOURCE_BARRIER_TYPE_TRANSITION, D3D12_RESOURCE_BARRIER_FLAG_NONE,
        {resource.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
         D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE}};
    commandList->ResourceBarrier(1, &barrier);
    return true;
}

} // namespace

struct DomDownToolPreview::Dx12FbxRenderer::Impl
{
    explicit Impl(std::function<void()> callback) : requestRedraw(std::move(callback)) {}

    std::function<void()> requestRedraw;
    std::vector<CpuBatch> batches;
    std::vector<CpuTexture> textures;
    Bounds bounds;
    std::array<float, 3> center{};
    float scale = 1.F;
    double yaw = -0.65;
    double pitch = 0.55;
    double zoom = 1.0;
    FbxPreviewMode mode = FbxPreviewMode::TexturedWireframe;
    uint64_t revision = 0;
    std::size_t vertexCount = 0;
    std::size_t triangleCount = 0;
    Dx12FbxState state;
};

DomDownToolPreview::Dx12FbxRenderer::Dx12FbxRenderer(std::function<void()> requestRedraw)
    : impl_(std::make_unique<Impl>(std::move(requestRedraw)))
{
}

DomDownToolPreview::Dx12FbxRenderer::~Dx12FbxRenderer() = default;

void DomDownToolPreview::Dx12FbxRenderer::clear()
{
    impl_->state.reset();
    impl_->batches.clear();
    impl_->textures.clear();
    impl_->center = {};
    impl_->scale = 1.F;
    impl_->bounds = {};
    impl_->vertexCount = 0;
    impl_->triangleCount = 0;
    impl_->revision++;
}

bool DomDownToolPreview::Dx12FbxRenderer::load(
    const std::vector<std::filesystem::path>& paths, std::string& error)
{
    registerSkiaImageCodecs();
    clear();
    std::unordered_map<std::string, uint32_t> textureIndices;
    bool loadedAny = false;
    const auto loadTexture = [&](const aiScene* scene, unsigned int materialIndex,
                                 const std::filesystem::path& fbxPath) -> uint32_t
    {
        if (!scene || materialIndex >= scene->mNumMaterials || impl_->textures.size() >= kMaximumTextures)
        {
            return std::numeric_limits<uint32_t>::max();
        }
        aiString name;
        if (scene->mMaterials[materialIndex]->GetTexture(aiTextureType_DIFFUSE, 0, &name) != AI_SUCCESS ||
            name.length == 0 || name.C_Str()[0] == '*')
        {
            return std::numeric_limits<uint32_t>::max();
        }
        const std::filesystem::path texturePath =
            (fbxPath.parent_path() / std::filesystem::u8path(name.C_Str())).lexically_normal();
        const std::string key = pathToUtf8(texturePath);
        if (const auto existing = textureIndices.find(key); existing != textureIndices.end())
        {
            return existing->second;
        }
        const sk_sp<SkImage> image = loadImage(texturePath);
        if (!image)
        {
            return std::numeric_limits<uint32_t>::max();
        }
        CpuTexture texture;
        texture.path = texturePath;
        texture.width = image->width();
        texture.height = image->height();
        texture.bc1 = encodeBc1(*image);
        if (texture.bc1.empty())
        {
            return std::numeric_limits<uint32_t>::max();
        }
        const uint32_t index = static_cast<uint32_t>(impl_->textures.size());
        impl_->textures.push_back(std::move(texture));
        textureIndices.emplace(key, index);
        return index;
    };
    for (const std::filesystem::path& path : paths)
    {
        Assimp::Importer importer;
        const aiScene* scene = importer.ReadFile(
            pathToUtf8(path), aiProcess_Triangulate | aiProcess_JoinIdenticalVertices |
                aiProcess_ImproveCacheLocality);
        if (!scene || !scene->mRootNode)
        {
            error = "无法读取 FBX：" + pathToUtf8(path) + "；" + importer.GetErrorString();
            continue;
        }
        const auto appendNode = [&](const auto& self, const aiNode* node,
                                    const aiMatrix4x4& parent) -> void
        {
            const aiMatrix4x4 transform = nodeTransform(node, parent);
            for (unsigned int meshIndex = 0; meshIndex < node->mNumMeshes; ++meshIndex)
            {
                const aiMesh* mesh = scene->mMeshes[node->mMeshes[meshIndex]];
                if (!mesh || !mesh->HasPositions() || !mesh->HasFaces() ||
                    impl_->vertexCount + mesh->mNumVertices > kMaximumVertices ||
                    impl_->triangleCount >= kMaximumTriangles)
                {
                    continue;
                }
                CpuBatch batch;
                batch.textureIndex = loadTexture(scene, mesh->mMaterialIndex, path);
                batch.vertices.reserve(mesh->mNumVertices);
                for (unsigned int vertexIndex = 0; vertexIndex < mesh->mNumVertices; ++vertexIndex)
                {
                    const aiVector3D point = transform * mesh->mVertices[vertexIndex];
                    const aiVector3D uv = mesh->HasTextureCoords(0)
                        ? mesh->mTextureCoords[0][vertexIndex] : aiVector3D{};
                    const std::array<float, 3> position{point.x, point.y, point.z};
                    includePoint(impl_->bounds, position);
                    batch.vertices.push_back({point.x, point.y, point.z, uv.x, uv.y});
                }
                for (unsigned int faceIndex = 0; faceIndex < mesh->mNumFaces; ++faceIndex)
                {
                    const aiFace& face = mesh->mFaces[faceIndex];
                    if (face.mNumIndices != 3 ||
                        impl_->triangleCount + batch.indices.size() / 3 >= kMaximumTriangles)
                    {
                        continue;
                    }
                    const uint32_t a = face.mIndices[0];
                    const uint32_t b = face.mIndices[1];
                    const uint32_t c = face.mIndices[2];
                    batch.indices.insert(batch.indices.end(), {a, b, c});
                    batch.edgeIndices.insert(batch.edgeIndices.end(), {a, b, b, c, c, a});
                }
                if (!batch.indices.empty())
                {
                    loadedAny = true;
                    impl_->vertexCount += batch.vertices.size();
                    impl_->triangleCount += batch.indices.size() / 3;
                    impl_->batches.push_back(std::move(batch));
                }
            }
            for (unsigned int childIndex = 0; childIndex < node->mNumChildren; ++childIndex)
            {
                self(self, node->mChildren[childIndex], transform);
            }
        };
        appendNode(appendNode, scene->mRootNode, aiMatrix4x4());
    }
    if (!loadedAny)
    {
        clear();
        if (error.empty())
        {
            error = "没有找到可预览的 FBX 三角网格。";
        }
        return false;
    }
    for (std::size_t index = 0; index < impl_->center.size(); ++index)
    {
        impl_->center[index] = (impl_->bounds.minimum[index] + impl_->bounds.maximum[index]) * 0.5F;
    }
    impl_->scale = std::max({impl_->bounds.maximum[0] - impl_->bounds.minimum[0],
                             impl_->bounds.maximum[1] - impl_->bounds.minimum[1],
                             impl_->bounds.maximum[2] - impl_->bounds.minimum[2], 1e-3F});
    for (CpuBatch& batch : impl_->batches)
    {
        for (GpuVertex& vertex : batch.vertices)
        {
            vertex.x = (vertex.x - impl_->center[0]) / impl_->scale;
            vertex.y = (vertex.y - impl_->center[1]) / impl_->scale;
            vertex.z = (vertex.z - impl_->center[2]) / impl_->scale;
        }
    }
    error.clear();
    ++impl_->revision;
    return true;
}

void DomDownToolPreview::Dx12FbxRenderer::setMode(FbxPreviewMode mode)
{
    impl_->mode = mode;
}

DomDownToolPreview::FbxPreviewMode DomDownToolPreview::Dx12FbxRenderer::mode() const
{
    return impl_->mode;
}

void DomDownToolPreview::Dx12FbxRenderer::rotateBy(double deltaX, double deltaY)
{
    impl_->yaw += deltaX * 0.008;
    impl_->pitch = std::clamp(impl_->pitch + deltaY * 0.008, -1.35, 1.35);
}

void DomDownToolPreview::Dx12FbxRenderer::zoomBy(double delta)
{
    impl_->zoom = std::clamp(impl_->zoom * std::pow(1.12, delta), 0.15, 12.0);
}

bool DomDownToolPreview::Dx12FbxRenderer::empty() const
{
    return impl_->batches.empty();
}

std::size_t DomDownToolPreview::Dx12FbxRenderer::vertexCount() const
{
    return impl_->vertexCount;
}

std::size_t DomDownToolPreview::Dx12FbxRenderer::triangleCount() const
{
    return impl_->triangleCount;
}

bool DomDownToolPreview::Dx12FbxRenderer::draw(SkCanvas& canvas, int width, int height,
                                               float /*dpiScale*/)
{
    if (empty() || width <= 0 || height <= 0)
    {
        return false;
    }
    SkSurface* surface = canvas.getSurface();
    if (!surface)
    {
        return false;
    }
    GrDirectContext* context = GrAsDirectContext(surface->recordingContext());
    if (!context || context->backend() != GrBackendApi::kDirect3D)
    {
        return false;
    }
    context->flushAndSubmit(surface, GrSyncCpu::kNo);
    surface->notifyContentWillChange(SkSurface::kRetain_ContentChangeMode);
    const GrBackendRenderTarget target =
        SkSurfaces::GetBackendRenderTarget(surface, SkSurface::BackendHandleAccess::kFlushWrite);
    const GrD3DTextureResourceInfo targetInfo =
        GrBackendRenderTargets::GetD3DTextureResourceInfo(target);
    ID3D12Resource* targetResource = targetInfo.fResource.get();
    auto* gpu = static_cast<GrD3DGpu*>(context->priv().getGpu());
    ID3D12CommandQueue* queue = gpu ? gpu->queue() : nullptr;
    if (!targetResource || !queue)
    {
        return false;
    }
    if (!impl_->state.device || impl_->state.queue.Get() != queue)
    {
        if (!initializeState(impl_->state, targetResource, queue))
        {
            return false;
        }
    }
    const D3D12_RESOURCE_DESC targetDescription = targetResource->GetDesc();
    if (!ensureDepth(impl_->state, static_cast<UINT>(targetDescription.Width), targetDescription.Height))
    {
        return false;
    }
    Frame& frame = impl_->state.frames[impl_->state.frameIndex];
    if (!impl_->state.waitForFence(frame.fenceValue) ||
        FAILED(frame.allocator->Reset()) || FAILED(frame.commandList->Reset(frame.allocator.Get(), nullptr)))
    {
        return false;
    }
    frame.fenceValue = 0;
    frame.retainedUpload = std::make_shared<UploadLifetime>();
    ID3D12GraphicsCommandList* commandList = frame.commandList.Get();
    if (impl_->state.loadedRevision != impl_->revision)
    {
        impl_->state.textures.clear();
        impl_->state.batches.clear();
        for (std::size_t index = 0; index < impl_->textures.size(); ++index)
        {
            ComPtr<ID3D12Resource> texture;
            ComPtr<ID3D12Resource> upload;
            if (!uploadTexture(impl_->state.device.Get(), commandList, impl_->textures[index], texture, upload))
            {
                return false;
            }
            impl_->state.textures.push_back(texture);
            frame.retainedUpload->uploads.push_back(upload);
            D3D12_SHADER_RESOURCE_VIEW_DESC view{};
            view.Format = DXGI_FORMAT_BC1_UNORM;
            view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            view.Texture2D.MipLevels = 1;
            D3D12_CPU_DESCRIPTOR_HANDLE handle = impl_->state.srvHeap->GetCPUDescriptorHandleForHeapStart();
            handle.ptr += static_cast<SIZE_T>(index) * impl_->state.descriptorSize;
            impl_->state.device->CreateShaderResourceView(texture.Get(), &view, handle);
        }
        for (const CpuBatch& batch : impl_->batches)
        {
            GpuBatch gpuBatch;
            gpuBatch.source = &batch;
            ComPtr<ID3D12Resource> vertexUpload;
            ComPtr<ID3D12Resource> indexUpload;
            ComPtr<ID3D12Resource> edgeUpload;
            if (!createBuffer(impl_->state.device.Get(), commandList, batch.vertices.data(),
                              batch.vertices.size() * sizeof(GpuVertex), gpuBatch.vertexBuffer,
                              vertexUpload) ||
                !createBuffer(impl_->state.device.Get(), commandList, batch.indices.data(),
                              batch.indices.size() * sizeof(uint32_t), gpuBatch.indexBuffer,
                              indexUpload) ||
                !createBuffer(impl_->state.device.Get(), commandList, batch.edgeIndices.data(),
                              batch.edgeIndices.size() * sizeof(uint32_t), gpuBatch.edgeBuffer,
                              edgeUpload))
            {
                return false;
            }
            frame.retainedUpload->uploads.push_back(vertexUpload);
            frame.retainedUpload->uploads.push_back(indexUpload);
            frame.retainedUpload->uploads.push_back(edgeUpload);
            gpuBatch.indexCount = static_cast<UINT>(batch.indices.size());
            gpuBatch.edgeIndexCount = static_cast<UINT>(batch.edgeIndices.size());
            impl_->state.batches.push_back(std::move(gpuBatch));
        }
        impl_->state.loadedRevision = impl_->revision;
    }
    D3D12_RESOURCE_STATES targetState = targetInfo.fResourceState;
    if (targetState != D3D12_RESOURCE_STATE_RENDER_TARGET)
    {
        const D3D12_RESOURCE_BARRIER barrier{
            D3D12_RESOURCE_BARRIER_TYPE_TRANSITION, D3D12_RESOURCE_BARRIER_FLAG_NONE,
            {targetResource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, targetState,
             D3D12_RESOURCE_STATE_RENDER_TARGET}};
        commandList->ResourceBarrier(1, &barrier);
    }
    const D3D12_CPU_DESCRIPTOR_HANDLE dsv =
        impl_->state.dsvHeap->GetCPUDescriptorHandleForHeapStart();
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv =
        impl_->state.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    impl_->state.device->CreateRenderTargetView(targetResource, nullptr, rtv);
    commandList->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
    commandList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.F, 0, 0, nullptr);
    const D3D12_VIEWPORT viewport{
        0.F, 0.F, static_cast<float>(width), static_cast<float>(height), 0.F, 1.F};
    const D3D12_RECT scissor{0, 0, width, height};
    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissor);
    commandList->SetDescriptorHeaps(1, impl_->state.srvHeap.GetAddressOf());
    commandList->SetGraphicsRootSignature(impl_->state.rootSignature.Get());
    const float sx = static_cast<float>(std::min(width, height)) * 0.82F *
        static_cast<float>(impl_->zoom);
    const float xScale = sx * 2.F / static_cast<float>(width);
    const float yScale = sx * 2.F / static_cast<float>(height);
    const double yawCos = std::cos(impl_->yaw);
    const double yawSin = std::sin(impl_->yaw);
    const double pitchCos = std::cos(impl_->pitch);
    const double pitchSin = std::sin(impl_->pitch);
    RootConstants constants{};
    constants.transform[0] = static_cast<float>(yawCos) * xScale;
    constants.transform[4] = -static_cast<float>(yawSin) * xScale;
    constants.transform[1] = -static_cast<float>(yawSin * pitchCos) * yScale;
    constants.transform[5] = -static_cast<float>(yawCos * pitchCos) * yScale;
    constants.transform[9] = static_cast<float>(pitchSin) * yScale;
    constants.transform[2] = static_cast<float>(pitchSin * yawSin) * 0.8F;
    constants.transform[6] = static_cast<float>(pitchSin * yawCos) * 0.8F;
    constants.transform[10] = static_cast<float>(pitchCos) * 0.8F;
    constants.transform[14] = 0.5F;
    const auto drawBatch = [&](const GpuBatch& batch, bool wire)
    {
        constants.textureIndex = wire || batch.source->textureIndex >= impl_->state.textures.size()
            ? std::numeric_limits<uint32_t>::max() : batch.source->textureIndex;
        commandList->SetGraphicsRoot32BitConstants(0, kRootConstantCount, &constants, 0);
        const D3D12_GPU_DESCRIPTOR_HANDLE heapHandle =
            impl_->state.srvHeap->GetGPUDescriptorHandleForHeapStart();
        commandList->SetGraphicsRootDescriptorTable(1, heapHandle);
        const D3D12_VERTEX_BUFFER_VIEW vertexView{
            batch.vertexBuffer->GetGPUVirtualAddress(),
            static_cast<UINT>(batch.source->vertices.size() * sizeof(GpuVertex)),
            sizeof(GpuVertex)};
        const D3D12_INDEX_BUFFER_VIEW indexView{
            (wire ? batch.edgeBuffer : batch.indexBuffer)->GetGPUVirtualAddress(),
            static_cast<UINT>((wire ? batch.source->edgeIndices.size() : batch.source->indices.size()) * sizeof(uint32_t)),
            DXGI_FORMAT_R32_UINT};
        commandList->IASetVertexBuffers(0, 1, &vertexView);
        commandList->IASetIndexBuffer(&indexView);
        commandList->IASetPrimitiveTopology(wire ? D3D_PRIMITIVE_TOPOLOGY_LINELIST
                                                 : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        commandList->SetPipelineState(wire ? impl_->state.wirePipeline.Get()
                                           : impl_->state.texturedPipeline.Get());
        commandList->DrawIndexedInstanced(wire ? batch.edgeIndexCount : batch.indexCount,
                                          1, 0, 0, 0);
    };
    for (const GpuBatch& batch : impl_->state.batches)
    {
        if (impl_->mode != FbxPreviewMode::Wireframe)
        {
            drawBatch(batch, false);
        }
        if (impl_->mode != FbxPreviewMode::Textured)
        {
            drawBatch(batch, true);
        }
    }
    if (targetState != D3D12_RESOURCE_STATE_RENDER_TARGET)
    {
        const D3D12_RESOURCE_BARRIER barrier{
            D3D12_RESOURCE_BARRIER_TYPE_TRANSITION, D3D12_RESOURCE_BARRIER_FLAG_NONE,
            {targetResource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
             D3D12_RESOURCE_STATE_RENDER_TARGET, targetState}};
        commandList->ResourceBarrier(1, &barrier);
    }
    if (FAILED(commandList->Close()))
    {
        return false;
    }
    ID3D12CommandList* lists[] = {commandList};
    impl_->state.queue->ExecuteCommandLists(1, lists);
    frame.fenceValue = ++impl_->state.fenceValue;
    if (FAILED(impl_->state.queue->Signal(impl_->state.fence.Get(), frame.fenceValue)))
    {
        return false;
    }
    impl_->state.frameIndex = (impl_->state.frameIndex + 1) % kFrameCount;
    return true;
}
