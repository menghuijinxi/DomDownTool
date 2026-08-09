#include "preview/Dx12GisRenderer.h"

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
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <fmt/format.h>

#include "include/core/SkCanvas.h"
#include "include/core/SkSurface.h"
#include "include/gpu/GpuTypes.h"
#include "include/gpu/ganesh/GrBackendSurface.h"
#include "include/gpu/ganesh/GrDirectContext.h"
#include "include/gpu/ganesh/GrRecordingContext.h"
#include "include/gpu/ganesh/SkSurfaceGanesh.h"
#include "include/gpu/ganesh/d3d/GrD3DBackendSurface.h"
#include "include/gpu/ganesh/d3d/GrD3DTypes.h"
#include "src/gpu/ganesh/GrDirectContextPriv.h"
#include "src/gpu/ganesh/d3d/GrD3DGpu.h"

#include "preview/SearchTrace.h"

using Microsoft::WRL::ComPtr;

namespace
{
using DomDownToolPreview::Dx12GisRenderFrame;
using DomDownToolPreview::Dx12GisRenderStats;
using DomDownToolPreview::Dx12GisDrawBatch;
using DomDownToolPreview::GisGeometryMesh;

constexpr DXGI_FORMAT kTargetFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
constexpr size_t kRootConstantCount = 16;
constexpr size_t kFrameContextCount = 3;
constexpr UINT kOverlayRtvIndex = static_cast<UINT>(kFrameContextCount);

struct BufferUploadFailure
{
    std::string_view bufferName;
    std::string_view operation;
    HRESULT result = S_OK;
    UINT64 byteSize = 0;
    bool directUploadFallback = false;
};

struct RootConstants
{
    float worldToScreen[4]{};
    float screenToClip[4]{};
    float color[4]{};
    float params[4]{};
};

static_assert(sizeof(RootConstants) == kRootConstantCount * sizeof(uint32_t));

const char* kFillVertexShader = R"(
cbuffer RootConstants : register(b0) {
    float4 worldToScreen;
    float4 screenToClip;
    float4 color;
    float4 params;
};
struct VSInput { float2 position : POSITION; };
struct VSOutput { float4 position : SV_POSITION; };
VSOutput main(VSInput input) {
    float2 screen = input.position * worldToScreen.xy + worldToScreen.zw;
    VSOutput output;
    output.position = float4(screen * screenToClip.xy + float2(-1.0, -1.0), 0.0, 1.0);
    output.position.y = -output.position.y;
    return output;
}
)";

const char* kLineVertexShader = R"(
cbuffer RootConstants : register(b0) {
    float4 worldToScreen;
    float4 screenToClip;
    float4 color;
    float4 params;
};
struct VSInput {
    float2 start : START;
    float2 end : END;
    float2 attributes : ATTRIBUTES;
};
struct VSOutput { float4 position : SV_POSITION; };
VSOutput main(VSInput input) {
    float2 start = input.start * worldToScreen.xy + worldToScreen.zw;
    float2 end = input.end * worldToScreen.xy + worldToScreen.zw;
    float2 direction = end - start;
    float distance = max(sqrt(dot(direction, direction)), 0.0001);
    direction /= distance;
    float2 normal = float2(-direction.y, direction.x);
    float halfWidth = params.x;
    start -= direction * halfWidth;
    end += direction * halfWidth;
    float2 screen = lerp(start, end, input.attributes.x) +
        normal * input.attributes.y * halfWidth;
    VSOutput output;
    output.position = float4(screen * screenToClip.xy + float2(-1.0, -1.0), 0.0, 1.0);
    output.position.y = -output.position.y;
    return output;
}
)";

const char* kPointVertexShader = R"(
cbuffer RootConstants : register(b0) {
    float4 worldToScreen;
    float4 screenToClip;
    float4 color;
    float4 params;
};
struct VSInput {
    float2 center : CENTER;
    float2 offset : OFFSET;
};
struct VSOutput {
    float4 position : SV_POSITION;
    float2 offset : TEXCOORD0;
};
VSOutput main(VSInput input) {
    float2 screen = input.center * worldToScreen.xy + worldToScreen.zw;
    screen += input.offset * params.y;
    VSOutput output;
    output.position = float4(screen * screenToClip.xy + float2(-1.0, -1.0), 0.0, 1.0);
    output.position.y = -output.position.y;
    output.offset = input.offset;
    return output;
}
)";

const char* kPixelShader = R"(
cbuffer RootConstants : register(b0) {
    float4 worldToScreen;
    float4 screenToClip;
    float4 color;
    float4 params;
};
struct PSInput { float4 position : SV_POSITION; };
float4 main(PSInput input) : SV_TARGET { return color; }
)";

const char* kPointPixelShader = R"(
cbuffer RootConstants : register(b0) {
    float4 worldToScreen;
    float4 screenToClip;
    float4 color;
    float4 params;
};
struct PSInput {
    float4 position : SV_POSITION;
    float2 offset : TEXCOORD0;
};
float4 main(PSInput input) : SV_TARGET {
    float radius = length(input.offset);
    float edgeWidth = max(fwidth(radius), 0.0001);
    float coverage = saturate((1.0 - radius) / edgeWidth + 0.5);
    clip(coverage - 0.001);
    return float4(color.rgb, color.a * coverage);
}
)";

const char* kCompositeVertexShader = R"(
struct VSOutput {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};
VSOutput main(uint vertexId : SV_VertexID) {
    VSOutput output;
    output.uv = float2((vertexId << 1) & 2, vertexId & 2);
    output.position = float4(output.uv * float2(2.0, -2.0) + float2(-1.0, 1.0),
                             0.0, 1.0);
    return output;
}
)";

const char* kCompositePixelShader = R"(
Texture2D overlayTexture : register(t0);
SamplerState overlaySampler : register(s0);
struct PSInput {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};
float4 main(PSInput input) : SV_TARGET {
    return overlayTexture.Sample(overlaySampler, input.uv);
}
)";

template <typename T>
bool createUploadBuffer(ID3D12Device* device,
                        std::span<const T> values,
                        ComPtr<ID3D12Resource>& upload,
                        BufferUploadFailure& failure)
{
    if (!device || values.empty())
    {
        failure.operation = "validate_upload_buffer";
        failure.result = E_INVALIDARG;
        return false;
    }
    const UINT64 byteSize = static_cast<UINT64>(values.size_bytes());
    failure.byteSize = byteSize;
    const D3D12_HEAP_PROPERTIES heapProperties{
        D3D12_HEAP_TYPE_UPLOAD, D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
        D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
    const D3D12_RESOURCE_DESC description{
        D3D12_RESOURCE_DIMENSION_BUFFER, 0, byteSize, 1, 1, 1, DXGI_FORMAT_UNKNOWN,
        {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE};
    const HRESULT createResult = device->CreateCommittedResource(
        &heapProperties, D3D12_HEAP_FLAG_NONE, &description,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload));
    if (FAILED(createResult))
    {
        failure.operation = "create_upload_resource";
        failure.result = createResult;
        return false;
    }
    void* mapped = nullptr;
    const D3D12_RANGE readRange{0, 0};
    const HRESULT mapResult = upload->Map(0, &readRange, &mapped);
    if (FAILED(mapResult) || !mapped)
    {
        failure.operation = "map_upload_resource";
        failure.result = FAILED(mapResult) ? mapResult : E_POINTER;
        upload.Reset();
        return false;
    }
    std::memcpy(mapped, values.data(), static_cast<size_t>(byteSize));
    upload->Unmap(0, nullptr);
    return true;
}

template <typename T>
bool createDefaultBuffer(ID3D12Device* device,
                         ID3D12GraphicsCommandList* commandList,
                         std::span<const T> values,
                         ComPtr<ID3D12Resource>& resource,
                         ComPtr<ID3D12Resource>& upload,
                         BufferUploadFailure& failure)
{
    if (!device || !commandList || values.empty())
    {
        failure.operation = "validate_default_buffer";
        failure.result = E_INVALIDARG;
        return false;
    }
    if (!createUploadBuffer(device, values, upload, failure))
    {
        return false;
    }
    const UINT64 byteSize = static_cast<UINT64>(values.size_bytes());
    const D3D12_HEAP_PROPERTIES heapProperties{
        D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
        D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
    const D3D12_RESOURCE_DESC description{
        D3D12_RESOURCE_DIMENSION_BUFFER, 0, byteSize, 1, 1, 1, DXGI_FORMAT_UNKNOWN,
        {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE};
    const HRESULT createResult = device->CreateCommittedResource(
        &heapProperties, D3D12_HEAP_FLAG_NONE, &description,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&resource));
    if (FAILED(createResult))
    {
        failure.operation = "create_default_resource";
        failure.result = createResult;
        // 默认堆创建失败时保留上传堆资源。UPLOAD 堆可直接作为只读顶点/索引缓冲，
        // 速度低于 DEFAULT 堆，但能避免 GIS 首帧因显存分配失败完全回退到 Skia。
        resource = std::move(upload);
        failure.directUploadFallback = resource != nullptr;
        return failure.directUploadFallback;
    }
    commandList->CopyBufferRegion(resource.Get(), 0, upload.Get(), 0, byteSize);
    return true;
}

ComPtr<ID3DBlob> compileShader(const char* source, const char* entryPoint, const char* target)
{
    ComPtr<ID3DBlob> code;
    ComPtr<ID3DBlob> errors;
    const UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
    const HRESULT result = D3DCompile(source, std::strlen(source), nullptr, nullptr, nullptr,
                                      entryPoint, target, flags, 0, &code, &errors);
    return SUCCEEDED(result) ? code : nullptr;
}

struct GpuMesh
{
    std::shared_ptr<const GisGeometryMesh> mesh;
    uint64_t revision = 0;
    size_t fillIndexCount = 0;
    size_t lineIndexCount = 0;
    size_t pointIndexCount = 0;
    ComPtr<ID3D12Resource> fillVertexBuffer;
    ComPtr<ID3D12Resource> fillIndexBuffer;
    ComPtr<ID3D12Resource> lineVertexBuffer;
    ComPtr<ID3D12Resource> lineIndexBuffer;
    ComPtr<ID3D12Resource> pointVertexBuffer;
    ComPtr<ID3D12Resource> pointIndexBuffer;
};

struct PendingTransition
{
    ComPtr<ID3D12Resource> resource;
    D3D12_RESOURCE_STATES finalState = D3D12_RESOURCE_STATE_COMMON;
};

struct UploadLifetime
{
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commandList;
    ComPtr<ID3D12Fence> fence;
    std::vector<ComPtr<ID3D12Resource>> bufferResources;
    std::vector<ComPtr<ID3D12Resource>> uploadResources;
    UINT64 fenceValue = 0;

    ~UploadLifetime()
    {
        if (!fence || fenceValue == 0 || fence->GetCompletedValue() >= fenceValue)
        {
            return;
        }
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (event)
        {
            if (SUCCEEDED(fence->SetEventOnCompletion(fenceValue, event)))
            {
                WaitForSingleObject(event, INFINITE);
            }
            CloseHandle(event);
        }
    }
};

struct UploadRequest
{
    uint64_t generation = 0;
    ComPtr<ID3D12Device> device;
    std::vector<std::shared_ptr<const GisGeometryMesh>> meshes;
};

struct UploadResult
{
    uint64_t generation = 0;
    bool success = false;
    UINT64 uploadedBytes = 0;
    int64_t submitMilliseconds = 0;
    std::string failure;
    std::vector<GpuMesh> meshes;
    std::vector<PendingTransition> transitions;
    std::shared_ptr<UploadLifetime> lifetime;
};

struct FrameContext
{
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commandList;
    std::vector<ComPtr<ID3D12Resource>> retainedResources;
    std::vector<std::shared_ptr<UploadLifetime>> retainedUploads;
    UINT64 fenceValue = 0;
};

UploadResult uploadMeshes(const UploadRequest& request)
{
    UploadResult result;
    result.generation = request.generation;
    result.lifetime = std::make_shared<UploadLifetime>();
    if (!request.device || request.meshes.empty())
    {
        result.failure = "invalid_upload_request";
        return result;
    }
    result.lifetime->device = request.device;

    D3D12_COMMAND_QUEUE_DESC queueDescription{};
    queueDescription.Type = D3D12_COMMAND_LIST_TYPE_COPY;
    if (FAILED(request.device->CreateCommandQueue(
            &queueDescription, IID_PPV_ARGS(&result.lifetime->queue))) ||
        FAILED(request.device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&result.lifetime->allocator))) ||
        FAILED(request.device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_COPY, result.lifetime->allocator.Get(), nullptr,
            IID_PPV_ARGS(&result.lifetime->commandList))) ||
        FAILED(request.device->CreateFence(
            0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&result.lifetime->fence))))
    {
        result.failure = "create_copy_context_failed";
        return result;
    }

    BufferUploadFailure failure;
    const auto uploadBuffer = [&]<typename T>(
                                  const std::vector<T>& values,
                                  std::string_view bufferName,
                                  D3D12_RESOURCE_STATES finalState,
                                  ComPtr<ID3D12Resource>& resource)
    {
        if (values.empty())
        {
            return true;
        }
        failure.bufferName = bufferName;
        failure.operation = {};
        failure.result = S_OK;
        failure.byteSize = static_cast<UINT64>(values.size() * sizeof(T));
        result.uploadedBytes += failure.byteSize;
        failure.directUploadFallback = false;
        ComPtr<ID3D12Resource> upload;
        if (!createDefaultBuffer(request.device.Get(), result.lifetime->commandList.Get(),
                                 std::span<const T>(values.data(), values.size()), resource,
                                 upload, failure))
        {
            return false;
        }
        if (failure.directUploadFallback)
        {
            DomDownToolPreview::writeSearchTrace(
                "GIS", "dx12_upload_heap_fallback",
                fmt::format("buffer={} bytes={} hresult=0x{:08X}", bufferName,
                            failure.byteSize, static_cast<uint32_t>(failure.result)));
        }
        else
        {
            result.transitions.push_back({resource, finalState});
            result.lifetime->uploadResources.push_back(std::move(upload));
        }
        result.lifetime->bufferResources.push_back(resource);
        return true;
    };

    result.meshes.reserve(request.meshes.size());
    for (const std::shared_ptr<const GisGeometryMesh>& mesh : request.meshes)
    {
        if (!mesh)
        {
            continue;
        }
        GpuMesh gpuMesh;
        gpuMesh.mesh = mesh;
        const bool uploaded =
            uploadBuffer(mesh->fillVertices, "fill_vertices",
                         D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
                         gpuMesh.fillVertexBuffer) &&
            uploadBuffer(mesh->fillIndices, "fill_indices",
                         D3D12_RESOURCE_STATE_INDEX_BUFFER,
                         gpuMesh.fillIndexBuffer) &&
            uploadBuffer(mesh->lineVertices, "line_vertices",
                         D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
                         gpuMesh.lineVertexBuffer) &&
            uploadBuffer(mesh->lineIndices, "line_indices",
                         D3D12_RESOURCE_STATE_INDEX_BUFFER,
                         gpuMesh.lineIndexBuffer) &&
            uploadBuffer(mesh->pointVertices, "point_vertices",
                         D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
                         gpuMesh.pointVertexBuffer) &&
            uploadBuffer(mesh->pointIndices, "point_indices",
                         D3D12_RESOURCE_STATE_INDEX_BUFFER,
                         gpuMesh.pointIndexBuffer);
        if (!uploaded)
        {
            result.failure = fmt::format(
                "buffer={} operation={} bytes={} hresult=0x{:08X}", failure.bufferName,
                failure.operation, failure.byteSize, static_cast<uint32_t>(failure.result));
            return result;
        }
        gpuMesh.fillIndexCount = mesh->fillIndices.size();
        gpuMesh.lineIndexCount = mesh->lineIndices.size();
        gpuMesh.pointIndexCount = mesh->pointIndices.size();
        gpuMesh.revision = mesh->revision;
        result.meshes.push_back(std::move(gpuMesh));
    }

    if (FAILED(result.lifetime->commandList->Close()))
    {
        result.failure = "close_copy_command_list_failed";
        return result;
    }
    ID3D12CommandList* commandLists[] = {result.lifetime->commandList.Get()};
    result.lifetime->queue->ExecuteCommandLists(1, commandLists);
    constexpr UINT64 kCopyFenceValue = 1;
    if (FAILED(result.lifetime->queue->Signal(result.lifetime->fence.Get(),
                                               kCopyFenceValue)))
    {
        result.failure = "signal_copy_fence_failed";
        return result;
    }
    result.lifetime->fenceValue = kCopyFenceValue;
    result.success = true;
    return result;
}

class GisUploadWorker
{
  public:
    explicit GisUploadWorker(std::function<void()> requestRedraw)
        : requestRedraw_(std::move(requestRedraw)), thread_([this] { run(); })
    {
    }

    ~GisUploadWorker()
    {
        {
            const std::lock_guard lock(mutex_);
            stopping_ = true;
            pending_.reset();
        }
        condition_.notify_one();
        if (thread_.joinable())
        {
            thread_.join();
        }
    }

    void submit(UploadRequest request)
    {
        {
            const std::lock_guard lock(mutex_);
            pending_ = std::move(request);
        }
        condition_.notify_one();
    }

    [[nodiscard]] std::optional<UploadResult> takeReady()
    {
        const std::lock_guard lock(mutex_);
        if (ready_.empty())
        {
            return std::nullopt;
        }
        std::optional<UploadResult> result{std::move(ready_.front())};
        ready_.pop_front();
        return result;
    }

    void cancelPending()
    {
        const std::lock_guard lock(mutex_);
        pending_.reset();
        ready_.clear();
    }

  private:
    void run()
    {
        while (true)
        {
            UploadRequest request;
            {
                std::unique_lock lock(mutex_);
                condition_.wait(lock, [this] { return stopping_ || pending_.has_value(); });
                if (stopping_)
                {
                    return;
                }
                request = std::move(*pending_);
                pending_.reset();
            }

            const auto uploadStart = std::chrono::steady_clock::now();
            UploadResult result = uploadMeshes(request);
            result.submitMilliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - uploadStart).count();
            {
                const std::lock_guard lock(mutex_);
                if (stopping_)
                {
                    return;
                }
                ready_.push_back(std::move(result));
            }
            if (requestRedraw_)
            {
                requestRedraw_();
            }
        }
    }

    std::mutex mutex_;
    std::condition_variable condition_;
    std::optional<UploadRequest> pending_;
    std::deque<UploadResult> ready_;
    std::function<void()> requestRedraw_;
    bool stopping_ = false;
    std::thread thread_;
};

struct Dx12GisRendererState
{
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> fence;
    HANDLE fenceEvent = nullptr;
    UINT64 fenceValue = 0;
    std::array<FrameContext, kFrameContextCount> frames;
    size_t nextFrame = 0;
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    ComPtr<ID3D12DescriptorHeap> srvHeap;
    UINT rtvDescriptorSize = 0;
    ComPtr<ID3D12RootSignature> rootSignature;
    ComPtr<ID3D12RootSignature> compositeRootSignature;
    ComPtr<ID3D12PipelineState> fillPipeline;
    ComPtr<ID3D12PipelineState> linePipeline;
    ComPtr<ID3D12PipelineState> pointPipeline;
    ComPtr<ID3D12PipelineState> compositePipeline;
    ComPtr<ID3D12Resource> overlayTarget;
    ComPtr<ID3D12Resource> resolvedOverlay;
    UINT overlayWidth = 0;
    UINT overlayHeight = 0;
    UINT sampleCount = 1;
    std::unordered_map<const GisGeometryMesh*, GpuMesh> gpuMeshes;
    std::unordered_set<const GisGeometryMesh*> requestedMeshes;
    std::vector<Dx12GisDrawBatch> lastReadyBatches;
    uint64_t uploadGeneration = 1;
    Dx12GisRenderStats stats;

    ~Dx12GisRendererState()
    {
        waitForAllFrames();
        if (fenceEvent)
        {
            CloseHandle(fenceEvent);
        }
    }

    void resetGpuResources()
    {
        waitForAllFrames();
        if (fenceEvent)
        {
            CloseHandle(fenceEvent);
            fenceEvent = nullptr;
        }
        gpuMeshes.clear();
        requestedMeshes.clear();
        lastReadyBatches.clear();
        resolvedOverlay.Reset();
        overlayTarget.Reset();
        compositePipeline.Reset();
        pointPipeline.Reset();
        linePipeline.Reset();
        fillPipeline.Reset();
        compositeRootSignature.Reset();
        rootSignature.Reset();
        srvHeap.Reset();
        rtvHeap.Reset();
        for (FrameContext& frame : frames)
        {
            frame.retainedUploads.clear();
            frame.retainedResources.clear();
            frame.commandList.Reset();
            frame.allocator.Reset();
            frame.fenceValue = 0;
        }
        queue.Reset();
        fence.Reset();
        device.Reset();
        fenceValue = 0;
        nextFrame = 0;
        overlayWidth = 0;
        overlayHeight = 0;
        sampleCount = 1;
        ++uploadGeneration;
        stats = {};
    }

    bool waitForFence(UINT64 value);

    bool waitForAllFrames()
    {
        bool success = true;
        for (FrameContext& frame : frames)
        {
            if (frame.fenceValue != 0 && !waitForFence(frame.fenceValue))
            {
                success = false;
            }
            frame.retainedUploads.clear();
            frame.retainedResources.clear();
            frame.fenceValue = 0;
        }
        return success;
    }
};

bool createPipelines(Dx12GisRendererState& state)
{
    if (!state.device)
    {
        return false;
    }
    ComPtr<ID3DBlob> fillVs = compileShader(kFillVertexShader, "main", "vs_5_0");
    ComPtr<ID3DBlob> lineVs = compileShader(kLineVertexShader, "main", "vs_5_0");
    ComPtr<ID3DBlob> pointVs = compileShader(kPointVertexShader, "main", "vs_5_0");
    ComPtr<ID3DBlob> ps = compileShader(kPixelShader, "main", "ps_5_0");
    ComPtr<ID3DBlob> pointPs = compileShader(kPointPixelShader, "main", "ps_5_0");
    ComPtr<ID3DBlob> compositeVs =
        compileShader(kCompositeVertexShader, "main", "vs_5_0");
    ComPtr<ID3DBlob> compositePs =
        compileShader(kCompositePixelShader, "main", "ps_5_0");
    if (!fillVs || !lineVs || !pointVs || !ps || !pointPs || !compositeVs ||
        !compositePs)
    {
        return false;
    }

    D3D12_ROOT_PARAMETER rootParameter{};
    rootParameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rootParameter.Constants.ShaderRegister = 0;
    rootParameter.Constants.RegisterSpace = 0;
    rootParameter.Constants.Num32BitValues = static_cast<UINT>(kRootConstantCount);
    rootParameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rootDescription{};
    rootDescription.NumParameters = 1;
    rootDescription.pParameters = &rootParameter;
    rootDescription.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> rootBlob;
    ComPtr<ID3DBlob> rootErrors;
    if (FAILED(D3D12SerializeRootSignature(&rootDescription,
                                           D3D_ROOT_SIGNATURE_VERSION_1,
                                           &rootBlob, &rootErrors)) ||
        FAILED(state.device->CreateRootSignature(0, rootBlob->GetBufferPointer(),
                                                  rootBlob->GetBufferSize(),
                                                  IID_PPV_ARGS(&state.rootSignature))))
    {
        return false;
    }

    D3D12_DESCRIPTOR_RANGE textureRange{};
    textureRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    textureRange.NumDescriptors = 1;
    textureRange.BaseShaderRegister = 0;
    textureRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    D3D12_ROOT_PARAMETER compositeParameter{};
    compositeParameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    compositeParameter.DescriptorTable.NumDescriptorRanges = 1;
    compositeParameter.DescriptorTable.pDescriptorRanges = &textureRange;
    compositeParameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC compositeRootDescription{};
    compositeRootDescription.NumParameters = 1;
    compositeRootDescription.pParameters = &compositeParameter;
    compositeRootDescription.NumStaticSamplers = 1;
    compositeRootDescription.pStaticSamplers = &sampler;
    ComPtr<ID3DBlob> compositeRootBlob;
    if (FAILED(D3D12SerializeRootSignature(
            &compositeRootDescription, D3D_ROOT_SIGNATURE_VERSION_1,
            &compositeRootBlob, &rootErrors)) ||
        FAILED(state.device->CreateRootSignature(
            0, compositeRootBlob->GetBufferPointer(), compositeRootBlob->GetBufferSize(),
            IID_PPV_ARGS(&state.compositeRootSignature))))
    {
        return false;
    }

    const D3D12_INPUT_ELEMENT_DESC fillInput[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
    const D3D12_INPUT_ELEMENT_DESC lineInput[] = {
        {"START", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"END", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"ATTRIBUTES", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 16,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
    const D3D12_INPUT_ELEMENT_DESC pointInput[] = {
        {"CENTER", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"OFFSET", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};

    const auto makePipeline = [&](const D3D12_INPUT_ELEMENT_DESC* input,
                                  UINT inputCount,
                                  ID3DBlob* vertexShader,
                                  ID3DBlob* pixelShader,
                                  ComPtr<ID3D12PipelineState>& pipeline)
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
        description.pRootSignature = state.rootSignature.Get();
        description.VS = {vertexShader->GetBufferPointer(), vertexShader->GetBufferSize()};
        description.PS = {pixelShader->GetBufferPointer(), pixelShader->GetBufferSize()};
        description.InputLayout = {input, inputCount};
        description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        description.NumRenderTargets = 1;
        description.RTVFormats[0] = kTargetFormat;
        description.SampleMask = D3D12_DEFAULT_SAMPLE_MASK;
        description.SampleDesc = {state.sampleCount, 0};
        description.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE, FALSE,
                                       D3D12_DEFAULT_DEPTH_BIAS, D3D12_DEFAULT_DEPTH_BIAS_CLAMP,
                                       D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS, TRUE,
                                       state.sampleCount > 1, FALSE,
                                       0, D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF};
        description.BlendState.RenderTarget[0].BlendEnable = TRUE;
        description.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
        description.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        description.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
        description.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
        description.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        description.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
        description.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        return SUCCEEDED(state.device->CreateGraphicsPipelineState(&description,
                                                                    IID_PPV_ARGS(&pipeline)));
    };
    if (!makePipeline(fillInput, static_cast<UINT>(std::size(fillInput)), fillVs.Get(), ps.Get(),
                      state.fillPipeline) ||
        !makePipeline(lineInput, static_cast<UINT>(std::size(lineInput)), lineVs.Get(), ps.Get(),
                      state.linePipeline) ||
        !makePipeline(pointInput, static_cast<UINT>(std::size(pointInput)), pointVs.Get(),
                      pointPs.Get(), state.pointPipeline))
    {
        return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC compositeDescription{};
    compositeDescription.pRootSignature = state.compositeRootSignature.Get();
    compositeDescription.VS = {compositeVs->GetBufferPointer(), compositeVs->GetBufferSize()};
    compositeDescription.PS = {compositePs->GetBufferPointer(), compositePs->GetBufferSize()};
    compositeDescription.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    compositeDescription.NumRenderTargets = 1;
    compositeDescription.RTVFormats[0] = kTargetFormat;
    compositeDescription.SampleMask = D3D12_DEFAULT_SAMPLE_MASK;
    compositeDescription.SampleDesc = {1, 0};
    compositeDescription.RasterizerState = {
        D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE, FALSE, D3D12_DEFAULT_DEPTH_BIAS,
        D3D12_DEFAULT_DEPTH_BIAS_CLAMP, D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS, TRUE,
        FALSE, FALSE, 0, D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF};
    D3D12_RENDER_TARGET_BLEND_DESC& compositeBlend =
        compositeDescription.BlendState.RenderTarget[0];
    compositeBlend.BlendEnable = TRUE;
    compositeBlend.SrcBlend = D3D12_BLEND_ONE;
    compositeBlend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    compositeBlend.BlendOp = D3D12_BLEND_OP_ADD;
    compositeBlend.SrcBlendAlpha = D3D12_BLEND_ONE;
    compositeBlend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    compositeBlend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    compositeBlend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    return SUCCEEDED(state.device->CreateGraphicsPipelineState(
        &compositeDescription, IID_PPV_ARGS(&state.compositePipeline)));
}

UINT selectSampleCount(ID3D12Device& device)
{
    for (const UINT sampleCount : {4U, 2U})
    {
        D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS qualityLevels{};
        qualityLevels.Format = kTargetFormat;
        qualityLevels.SampleCount = sampleCount;
        qualityLevels.Flags = D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE;
        if (SUCCEEDED(device.CheckFeatureSupport(
                D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &qualityLevels,
                sizeof(qualityLevels))) &&
            qualityLevels.NumQualityLevels > 0)
        {
            return sampleCount;
        }
    }
    return 1;
}

bool initializeDevice(Dx12GisRendererState& state,
                      ID3D12Resource* targetResource,
                      ID3D12CommandQueue* commandQueue)
{
    if (!targetResource || !commandQueue)
    {
        return false;
    }
    ComPtr<ID3D12Device> device;
    if (FAILED(targetResource->GetDevice(IID_PPV_ARGS(&device))))
    {
        return false;
    }
    state.resetGpuResources();
    state.device = std::move(device);
    state.queue = commandQueue;
    state.sampleCount = selectSampleCount(*state.device.Get());
    for (FrameContext& frame : state.frames)
    {
        if (FAILED(state.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                         IID_PPV_ARGS(&frame.allocator))) ||
            FAILED(state.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                    frame.allocator.Get(), nullptr,
                                                    IID_PPV_ARGS(&frame.commandList))) ||
            FAILED(frame.commandList->Close()))
        {
            state.resetGpuResources();
            return false;
        }
    }
    if (FAILED(state.device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                         IID_PPV_ARGS(&state.fence))))
    {
        state.resetGpuResources();
        return false;
    }
    state.fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!state.fenceEvent)
    {
        state.resetGpuResources();
        return false;
    }
    const D3D12_DESCRIPTOR_HEAP_DESC heapDescription{
        D3D12_DESCRIPTOR_HEAP_TYPE_RTV, static_cast<UINT>(kFrameContextCount + 1),
        D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    if (FAILED(state.device->CreateDescriptorHeap(&heapDescription,
                                                  IID_PPV_ARGS(&state.rtvHeap))))
    {
        state.resetGpuResources();
        return false;
    }
    state.rtvDescriptorSize = state.device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    const D3D12_DESCRIPTOR_HEAP_DESC srvHeapDescription{
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1,
        D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    if (FAILED(state.device->CreateDescriptorHeap(
            &srvHeapDescription, IID_PPV_ARGS(&state.srvHeap))))
    {
        state.resetGpuResources();
        return false;
    }
    if (!createPipelines(state))
    {
        state.resetGpuResources();
        return false;
    }
    return true;
}

bool ensureOverlayTargets(Dx12GisRendererState& state, UINT width, UINT height)
{
    if (state.overlayTarget && state.overlayWidth == width &&
        state.overlayHeight == height)
    {
        return true;
    }
    if (width == 0 || height == 0 || !state.waitForAllFrames())
    {
        return false;
    }
    state.overlayTarget.Reset();
    state.resolvedOverlay.Reset();

    const D3D12_HEAP_PROPERTIES heapProperties{
        D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
        D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
    D3D12_RESOURCE_DESC targetDescription{};
    targetDescription.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    targetDescription.Width = width;
    targetDescription.Height = height;
    targetDescription.DepthOrArraySize = 1;
    targetDescription.MipLevels = 1;
    targetDescription.Format = kTargetFormat;
    targetDescription.SampleDesc = {state.sampleCount, 0};
    targetDescription.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    targetDescription.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    const D3D12_CLEAR_VALUE clearValue{kTargetFormat, {0.F, 0.F, 0.F, 0.F}};
    if (FAILED(state.device->CreateCommittedResource(
            &heapProperties, D3D12_HEAP_FLAG_NONE, &targetDescription,
            D3D12_RESOURCE_STATE_RENDER_TARGET, &clearValue,
            IID_PPV_ARGS(&state.overlayTarget))))
    {
        return false;
    }

    if (state.sampleCount > 1)
    {
        targetDescription.SampleDesc = {1, 0};
        targetDescription.Flags = D3D12_RESOURCE_FLAG_NONE;
        if (FAILED(state.device->CreateCommittedResource(
                &heapProperties, D3D12_HEAP_FLAG_NONE, &targetDescription,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                IID_PPV_ARGS(&state.resolvedOverlay))))
        {
            state.overlayTarget.Reset();
            return false;
        }
    }

    D3D12_CPU_DESCRIPTOR_HANDLE overlayRtv =
        state.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    overlayRtv.ptr += static_cast<SIZE_T>(kOverlayRtvIndex) * state.rtvDescriptorSize;
    state.device->CreateRenderTargetView(state.overlayTarget.Get(), nullptr, overlayRtv);

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDescription{};
    srvDescription.Format = kTargetFormat;
    srvDescription.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDescription.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDescription.Texture2D.MipLevels = 1;
    state.device->CreateShaderResourceView(
        state.sampleCount > 1 ? state.resolvedOverlay.Get() : state.overlayTarget.Get(),
        &srvDescription, state.srvHeap->GetCPUDescriptorHandleForHeapStart());
    state.overlayTarget->SetName(L"DomDownTool GIS MSAA target");
    if (state.resolvedOverlay)
    {
        state.resolvedOverlay->SetName(L"DomDownTool GIS resolved overlay");
    }
    state.overlayWidth = width;
    state.overlayHeight = height;
    return true;
}

bool Dx12GisRendererState::waitForFence(UINT64 value)
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

FrameContext* beginFrame(Dx12GisRendererState& state)
{
    FrameContext& frame = state.frames[state.nextFrame];
    state.nextFrame = (state.nextFrame + 1) % state.frames.size();
    if (!state.waitForFence(frame.fenceValue))
    {
        return nullptr;
    }
    frame.retainedUploads.clear();
    frame.retainedResources.clear();
    frame.fenceValue = 0;
    if (FAILED(frame.allocator->Reset()) ||
        FAILED(frame.commandList->Reset(frame.allocator.Get(), nullptr)))
    {
        return nullptr;
    }
    return &frame;
}

bool signalFrame(Dx12GisRendererState& state, FrameContext& frame)
{
    const UINT64 value = ++state.fenceValue;
    if (FAILED(state.queue->Signal(state.fence.Get(), value)))
    {
        return false;
    }
    frame.fenceValue = value;
    return true;
}

void setConstants(ID3D12GraphicsCommandList* commandList,
                  const Dx12GisRenderFrame& frame,
                  const GisGeometryMesh& mesh,
                  float red,
                  float green,
                  float blue,
                  float alpha,
                  float size)
{
    const float mapWidth = static_cast<float>(std::max(1, frame.width));
    const float mapHeight = static_cast<float>(std::max(1, frame.height));
    const double sourceScale = std::exp2(
        static_cast<double>(frame.transform.sourceZoom) -
        static_cast<double>(mesh.sourceZoom));
    const DomDownToolPreview::ViewportPoint meshOriginScreen =
        frame.transform.screenPointForSourcePoint(
            {mesh.sourceOriginX * sourceScale, mesh.sourceOriginY * sourceScale});
    RootConstants constants{};
    constants.worldToScreen[0] =
        static_cast<float>(frame.transform.displayScale * sourceScale);
    constants.worldToScreen[1] =
        static_cast<float>(frame.transform.displayScale * sourceScale);
    constants.worldToScreen[2] = static_cast<float>(meshOriginScreen.x);
    constants.worldToScreen[3] = static_cast<float>(meshOriginScreen.y);
    constants.screenToClip[0] = 2.F / mapWidth;
    constants.screenToClip[1] = 2.F / mapHeight;
    constants.screenToClip[2] = static_cast<float>(frame.clipTop);
    constants.screenToClip[3] = static_cast<float>(frame.clipBottom);
    constants.color[0] = red;
    constants.color[1] = green;
    constants.color[2] = blue;
    constants.color[3] = std::max(0.F, std::min(1.F, alpha));
    constants.params[0] = size;
    constants.params[1] = size;
    commandList->SetGraphicsRoot32BitConstants(0, static_cast<UINT>(kRootConstantCount),
                                                &constants, 0);
}
} // namespace

struct DomDownToolPreview::Dx12GisRenderer::Impl
{
    explicit Impl(std::function<void()> requestRedraw)
        : uploader(std::move(requestRedraw))
    {
    }

    Dx12GisRendererState state;
    GisUploadWorker uploader;
    std::string lastFailure;
    bool successLogged = false;

    bool reportFailure(std::string_view reason, std::string_view detail = {})
    {
        const std::string failure = detail.empty()
            ? std::string(reason)
            : fmt::format("{} {}", reason, detail);
        if (lastFailure != failure)
        {
            lastFailure = failure;
            writeSearchTrace(
                "GIS", "dx12_draw_failed",
                detail.empty() ? fmt::format("reason={}", reason)
                               : fmt::format("reason={} {}", reason, detail));
        }
        return false;
    }

    void reportSuccess(const Dx12GisRenderFrame& frame,
                       size_t drawCallCount,
                       size_t uploadedMeshCount,
                       D3D12_RESOURCE_STATES targetState)
    {
        lastFailure.clear();
        if (successLogged)
        {
            return;
        }
        successLogged = true;
        writeSearchTrace(
            "GIS", "dx12_draw_succeeded",
            fmt::format("batches={} draw_calls={} uploaded_meshes={} target_state={} msaa={}",
                        frame.batches.size(), drawCallCount, uploadedMeshCount,
                        static_cast<unsigned>(targetState), state.sampleCount));
    }
};

DomDownToolPreview::Dx12GisRenderer::Dx12GisRenderer(
    std::function<void()> requestRedraw)
    : impl_(std::make_unique<Impl>(std::move(requestRedraw)))
{
}

DomDownToolPreview::Dx12GisRenderer::~Dx12GisRenderer() = default;

bool DomDownToolPreview::Dx12GisRenderer::SupportsCanvas(SkCanvas& canvas) const
{
    SkSurface* surface = canvas.getSurface();
    if (!surface)
    {
        return false;
    }
    GrDirectContext* directContext = GrAsDirectContext(surface->recordingContext());
    return directContext != nullptr && directContext->backend() == GrBackendApi::kDirect3D;
}

bool DomDownToolPreview::Dx12GisRenderer::Draw(
    SkCanvas& canvas, const Dx12GisRenderFrame& frame)
{
    if (!impl_ || frame.width <= 0 || frame.height <= 0)
    {
        return impl_ ? impl_->reportFailure("invalid_frame") : false;
    }
    SkSurface* surface = canvas.getSurface();
    if (!surface)
    {
        return impl_->reportFailure("missing_surface");
    }
    GrDirectContext* directContext =
        GrAsDirectContext(surface->recordingContext());
    if (!directContext || directContext->backend() != GrBackendApi::kDirect3D)
    {
        return impl_->reportFailure("unsupported_canvas");
    }

    // 与 Skia 共用同一 D3D12 queue，队列顺序保证底图先于 GIS 执行，不阻塞 UI 线程。
    directContext->flushAndSubmit(surface, GrSyncCpu::kNo);
    // GIS 会保留底图并写入同一个 back buffer，必须显式告知 Skia 此后的外部写入。
    surface->notifyContentWillChange(SkSurface::kRetain_ContentChangeMode);
    const GrBackendRenderTarget backendTarget =
        SkSurfaces::GetBackendRenderTarget(surface, SkSurface::BackendHandleAccess::kFlushWrite);
    if (!backendTarget.isValid())
    {
        return impl_->reportFailure("invalid_backend_target");
    }
    const GrD3DTextureResourceInfo targetInfo =
        GrBackendRenderTargets::GetD3DTextureResourceInfo(backendTarget);
    ID3D12Resource* targetResource = targetInfo.fResource.get();
    if (!targetResource)
    {
        return impl_->reportFailure("missing_target_resource");
    }
    auto* d3dGpu = static_cast<GrD3DGpu*>(directContext->priv().getGpu());
    ID3D12CommandQueue* commandQueue = d3dGpu != nullptr ? d3dGpu->queue() : nullptr;
    if (!commandQueue)
    {
        return impl_->reportFailure("missing_command_queue");
    }
    if (!impl_->state.device || impl_->state.queue.Get() != commandQueue || [&] {
            ComPtr<ID3D12Device> targetDevice;
            return FAILED(targetResource->GetDevice(IID_PPV_ARGS(&targetDevice))) ||
                targetDevice.Get() != impl_->state.device.Get();
        }())
    {
        if (!initializeDevice(impl_->state, targetResource, commandQueue))
        {
            return impl_->reportFailure("device_initialization_failed");
        }
        impl_->uploader.cancelPending();
    }

    const D3D12_RESOURCE_DESC targetDescription = targetResource->GetDesc();
    const UINT targetWidth = static_cast<UINT>(std::min<UINT64>(
        targetDescription.Width, std::numeric_limits<UINT>::max()));
    const UINT targetHeight = targetDescription.Height;
    if (!ensureOverlayTargets(impl_->state, targetWidth, targetHeight))
    {
        return impl_->reportFailure("create_overlay_targets_failed");
    }
    FrameContext* commandFrame = beginFrame(impl_->state);
    if (!commandFrame)
    {
        return impl_->reportFailure("begin_frame_failed");
    }
    ID3D12GraphicsCommandList* commandList = commandFrame->commandList.Get();
    const size_t frameIndex = static_cast<size_t>(commandFrame - impl_->state.frames.data());
    D3D12_CPU_DESCRIPTOR_HANDLE rtv =
        impl_->state.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += frameIndex * impl_->state.rtvDescriptorSize;
    impl_->state.rtvHeap->SetName(L"DomDownTool GIS RTV");
    impl_->state.device->CreateRenderTargetView(targetResource, nullptr, rtv);
    D3D12_CPU_DESCRIPTOR_HANDLE overlayRtv =
        impl_->state.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    overlayRtv.ptr += static_cast<SIZE_T>(kOverlayRtvIndex) *
        impl_->state.rtvDescriptorSize;

    std::unordered_set<const GisGeometryMesh*> residentMeshes;
    residentMeshes.reserve(frame.residentMeshes.size());
    for (const std::shared_ptr<const GisGeometryMesh>& mesh : frame.residentMeshes)
    {
        residentMeshes.insert(mesh.get());
    }
    std::erase_if(impl_->state.requestedMeshes,
                  [&](const GisGeometryMesh* mesh) { return !residentMeshes.contains(mesh); });

    size_t uploadedMeshCount = 0;
    if (std::optional<UploadResult> uploadResult = impl_->uploader.takeReady())
    {
        for (const GpuMesh& mesh : uploadResult->meshes)
        {
            impl_->state.requestedMeshes.erase(mesh.mesh.get());
        }
        if (uploadResult->generation == impl_->state.uploadGeneration)
        {
            if (!uploadResult->success)
            {
                impl_->state.requestedMeshes.clear();
                writeSearchTrace("GIS", "dx12_background_upload_failed",
                                 uploadResult->failure);
            }
            else if (FAILED(impl_->state.queue->Wait(
                         uploadResult->lifetime->fence.Get(),
                         uploadResult->lifetime->fenceValue)))
            {
                return impl_->reportFailure("wait_for_copy_queue_failed");
            }
            else
            {
                std::vector<D3D12_RESOURCE_BARRIER> barriers;
                barriers.reserve(uploadResult->transitions.size());
                for (const PendingTransition& transition : uploadResult->transitions)
                {
                    barriers.push_back({
                        D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
                        D3D12_RESOURCE_BARRIER_FLAG_NONE,
                        {transition.resource.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                         D3D12_RESOURCE_STATE_COPY_DEST, transition.finalState}});
                }
                if (!barriers.empty())
                {
                    commandList->ResourceBarrier(static_cast<UINT>(barriers.size()),
                                                 barriers.data());
                }
                for (GpuMesh& mesh : uploadResult->meshes)
                {
                    if (!residentMeshes.contains(mesh.mesh.get()))
                    {
                        continue;
                    }
                    impl_->state.gpuMeshes.insert_or_assign(mesh.mesh.get(), std::move(mesh));
                    ++impl_->state.stats.uploadedMeshes;
                    ++uploadedMeshCount;
                }
                commandFrame->retainedUploads.push_back(std::move(uploadResult->lifetime));
                writeSearchTrace(
                    "GIS", "dx12_background_upload_ready",
                    fmt::format("meshes={} bytes={} submit_elapsed_ms={}",
                                uploadedMeshCount, uploadResult->uploadedBytes,
                                uploadResult->submitMilliseconds));
            }
        }
        else if (uploadResult->success && uploadResult->lifetime &&
                 uploadResult->lifetime->device.Get() == impl_->state.device.Get() &&
                 SUCCEEDED(impl_->state.queue->Wait(
                     uploadResult->lifetime->fence.Get(),
                     uploadResult->lifetime->fenceValue)))
        {
            commandFrame->retainedUploads.push_back(std::move(uploadResult->lifetime));
        }
    }

    std::vector<std::shared_ptr<const GisGeometryMesh>> meshesToUpload;
    for (const Dx12GisDrawBatch& batch : frame.batches)
    {
        if (!batch.mesh)
        {
            continue;
        }
        const auto gpuMesh = impl_->state.gpuMeshes.find(batch.mesh.get());
        if ((gpuMesh == impl_->state.gpuMeshes.end() ||
             gpuMesh->second.revision != batch.mesh->revision) &&
            impl_->state.requestedMeshes.insert(batch.mesh.get()).second)
        {
            meshesToUpload.push_back(batch.mesh);
        }
    }
    if (!meshesToUpload.empty())
    {
        impl_->uploader.submit(
            {impl_->state.uploadGeneration, impl_->state.device, std::move(meshesToUpload)});
    }

    const auto batchIsReady = [&](const Dx12GisDrawBatch& batch)
    {
        if (!batch.mesh)
        {
            return false;
        }
        const auto gpuMesh = impl_->state.gpuMeshes.find(batch.mesh.get());
        return gpuMesh != impl_->state.gpuMeshes.end() &&
            gpuMesh->second.revision == batch.mesh->revision;
    };
    const bool currentFrameReady = frame.meshSetReady &&
        std::all_of(frame.batches.begin(), frame.batches.end(), batchIsReady);
    const std::vector<Dx12GisDrawBatch>* batchesToDraw = &impl_->state.lastReadyBatches;
    if (currentFrameReady)
    {
        impl_->state.lastReadyBatches = frame.batches;
        batchesToDraw = &impl_->state.lastReadyBatches;

        for (auto iterator = impl_->state.gpuMeshes.begin();
             iterator != impl_->state.gpuMeshes.end();)
        {
            if (residentMeshes.contains(iterator->first))
            {
                ++iterator;
                continue;
            }
            GpuMesh& retired = iterator->second;
            const ComPtr<ID3D12Resource> resources[] = {
                retired.fillVertexBuffer, retired.fillIndexBuffer,
                retired.lineVertexBuffer, retired.lineIndexBuffer,
                retired.pointVertexBuffer, retired.pointIndexBuffer};
            for (const ComPtr<ID3D12Resource>& resource : resources)
            {
                if (resource)
                {
                    commandFrame->retainedResources.push_back(resource);
                }
            }
            iterator = impl_->state.gpuMeshes.erase(iterator);
        }
    }

    const D3D12_VIEWPORT viewport{0.F, 0.F, static_cast<float>(targetDescription.Width),
                                  static_cast<float>(targetDescription.Height), 0.F, 1.F};
    const float clipBottom =
        std::min(frame.clipBottom, static_cast<float>(targetDescription.Height));
    const D3D12_RECT scissor{0, static_cast<LONG>(std::max(0.F, frame.clipTop)),
                             static_cast<LONG>(targetDescription.Width),
                             static_cast<LONG>(clipBottom)};
    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissor);
    commandList->OMSetRenderTargets(1, &overlayRtv, FALSE, nullptr);
    const float transparent[] = {0.F, 0.F, 0.F, 0.F};
    commandList->ClearRenderTargetView(overlayRtv, transparent, 0, nullptr);
    commandList->SetGraphicsRootSignature(impl_->state.rootSignature.Get());
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    size_t submittedDrawCalls = 0;
    const auto drawBatch = [&](const Dx12GisDrawBatch& batch, const GpuMesh& gpuMesh)
    {
        const GisGeometryMesh& mesh = *gpuMesh.mesh;
        setConstants(commandList, frame, mesh, batch.red, batch.green,
                     batch.blue,
                     batch.alpha, batch.lineWidth);
        if (gpuMesh.fillIndexCount != 0 && gpuMesh.fillVertexBuffer &&
            gpuMesh.fillIndexBuffer)
        {
            const D3D12_VERTEX_BUFFER_VIEW vertexView{
                gpuMesh.fillVertexBuffer->GetGPUVirtualAddress(),
                static_cast<UINT>(mesh.fillVertices.size() * sizeof(GisMeshVertex)),
                static_cast<UINT>(sizeof(GisMeshVertex))};
            const D3D12_INDEX_BUFFER_VIEW indexView{
                gpuMesh.fillIndexBuffer->GetGPUVirtualAddress(),
                static_cast<UINT>(mesh.fillIndices.size() * sizeof(uint32_t)),
                DXGI_FORMAT_R32_UINT};
            commandList->SetPipelineState(impl_->state.fillPipeline.Get());
            commandList->IASetVertexBuffers(0, 1, &vertexView);
            commandList->IASetIndexBuffer(&indexView);
            commandList->DrawIndexedInstanced(
                static_cast<UINT>(gpuMesh.fillIndexCount), 1, 0, 0, 0);
            ++impl_->state.stats.fillDrawCalls;
            ++submittedDrawCalls;
        }

        setConstants(commandList, frame, mesh, batch.red, batch.green,
                     batch.blue,
                     batch.alpha, batch.lineWidth * 0.5F);
        if (gpuMesh.lineIndexCount != 0 && gpuMesh.lineVertexBuffer &&
            gpuMesh.lineIndexBuffer)
        {
            const D3D12_VERTEX_BUFFER_VIEW vertexView{
                gpuMesh.lineVertexBuffer->GetGPUVirtualAddress(),
                static_cast<UINT>(mesh.lineVertices.size() * sizeof(GisLineVertex)),
                static_cast<UINT>(sizeof(GisLineVertex))};
            const D3D12_INDEX_BUFFER_VIEW indexView{
                gpuMesh.lineIndexBuffer->GetGPUVirtualAddress(),
                static_cast<UINT>(mesh.lineIndices.size() * sizeof(uint32_t)),
                DXGI_FORMAT_R32_UINT};
            commandList->SetPipelineState(impl_->state.linePipeline.Get());
            commandList->IASetVertexBuffers(0, 1, &vertexView);
            commandList->IASetIndexBuffer(&indexView);
            commandList->DrawIndexedInstanced(
                static_cast<UINT>(gpuMesh.lineIndexCount), 1, 0, 0, 0);
            ++impl_->state.stats.lineDrawCalls;
            ++submittedDrawCalls;
        }

        setConstants(commandList, frame, mesh, batch.red, batch.green,
                     batch.blue,
                     batch.alpha, batch.pointRadius);
        if (gpuMesh.pointIndexCount != 0 && gpuMesh.pointVertexBuffer &&
            gpuMesh.pointIndexBuffer)
        {
            const D3D12_VERTEX_BUFFER_VIEW vertexView{
                gpuMesh.pointVertexBuffer->GetGPUVirtualAddress(),
                static_cast<UINT>(mesh.pointVertices.size() * sizeof(GisPointVertex)),
                static_cast<UINT>(sizeof(GisPointVertex))};
            const D3D12_INDEX_BUFFER_VIEW indexView{
                gpuMesh.pointIndexBuffer->GetGPUVirtualAddress(),
                static_cast<UINT>(mesh.pointIndices.size() * sizeof(uint32_t)),
                DXGI_FORMAT_R32_UINT};
            commandList->SetPipelineState(impl_->state.pointPipeline.Get());
            commandList->IASetVertexBuffers(0, 1, &vertexView);
            commandList->IASetIndexBuffer(&indexView);
            commandList->DrawIndexedInstanced(
                static_cast<UINT>(gpuMesh.pointIndexCount), 1, 0, 0, 0);
            ++impl_->state.stats.pointDrawCalls;
            ++submittedDrawCalls;
        }
    };
    for (const Dx12GisDrawBatch& batch : *batchesToDraw)
    {
        if (!batch.mesh)
        {
            continue;
        }
        const auto gpuMesh = impl_->state.gpuMeshes.find(batch.mesh.get());
        if (gpuMesh != impl_->state.gpuMeshes.end() &&
            gpuMesh->second.revision == batch.mesh->revision)
        {
            drawBatch(batch, gpuMesh->second);
        }
    }

    if (impl_->state.sampleCount > 1)
    {
        const D3D12_RESOURCE_BARRIER resolveBarriers[] = {
            {D3D12_RESOURCE_BARRIER_TYPE_TRANSITION, D3D12_RESOURCE_BARRIER_FLAG_NONE,
             {impl_->state.overlayTarget.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
              D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RESOLVE_SOURCE}},
            {D3D12_RESOURCE_BARRIER_TYPE_TRANSITION, D3D12_RESOURCE_BARRIER_FLAG_NONE,
             {impl_->state.resolvedOverlay.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
              D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
              D3D12_RESOURCE_STATE_RESOLVE_DEST}}};
        commandList->ResourceBarrier(static_cast<UINT>(std::size(resolveBarriers)),
                                     resolveBarriers);
        commandList->ResolveSubresource(impl_->state.resolvedOverlay.Get(), 0,
                                        impl_->state.overlayTarget.Get(), 0, kTargetFormat);
        const D3D12_RESOURCE_BARRIER sampleBarriers[] = {
            {D3D12_RESOURCE_BARRIER_TYPE_TRANSITION, D3D12_RESOURCE_BARRIER_FLAG_NONE,
             {impl_->state.overlayTarget.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
              D3D12_RESOURCE_STATE_RESOLVE_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET}},
            {D3D12_RESOURCE_BARRIER_TYPE_TRANSITION, D3D12_RESOURCE_BARRIER_FLAG_NONE,
             {impl_->state.resolvedOverlay.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
              D3D12_RESOURCE_STATE_RESOLVE_DEST,
              D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE}}};
        commandList->ResourceBarrier(static_cast<UINT>(std::size(sampleBarriers)),
                                     sampleBarriers);
    }
    else
    {
        const D3D12_RESOURCE_BARRIER sampleBarrier{
            D3D12_RESOURCE_BARRIER_TYPE_TRANSITION, D3D12_RESOURCE_BARRIER_FLAG_NONE,
            {impl_->state.overlayTarget.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
             D3D12_RESOURCE_STATE_RENDER_TARGET,
             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE}};
        commandList->ResourceBarrier(1, &sampleBarrier);
    }

    D3D12_RESOURCE_STATES targetState = targetInfo.fResourceState;
    if (targetState != D3D12_RESOURCE_STATE_RENDER_TARGET)
    {
        const D3D12_RESOURCE_BARRIER targetBarrier{
            D3D12_RESOURCE_BARRIER_TYPE_TRANSITION, D3D12_RESOURCE_BARRIER_FLAG_NONE,
            {targetResource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, targetState,
             D3D12_RESOURCE_STATE_RENDER_TARGET}};
        commandList->ResourceBarrier(1, &targetBarrier);
        targetState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
    commandList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    commandList->SetPipelineState(impl_->state.compositePipeline.Get());
    commandList->SetGraphicsRootSignature(impl_->state.compositeRootSignature.Get());
    ID3D12DescriptorHeap* descriptorHeaps[] = {impl_->state.srvHeap.Get()};
    commandList->SetDescriptorHeaps(1, descriptorHeaps);
    commandList->SetGraphicsRootDescriptorTable(
        0, impl_->state.srvHeap->GetGPUDescriptorHandleForHeapStart());
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList->DrawInstanced(3, 1, 0, 0);

    std::array<D3D12_RESOURCE_BARRIER, 2> finalBarriers{};
    UINT finalBarrierCount = 0;
    if (impl_->state.sampleCount == 1)
    {
        finalBarriers[finalBarrierCount++] = {
            D3D12_RESOURCE_BARRIER_TYPE_TRANSITION, D3D12_RESOURCE_BARRIER_FLAG_NONE,
            {impl_->state.overlayTarget.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
             D3D12_RESOURCE_STATE_RENDER_TARGET}};
    }
    if (targetState != targetInfo.fResourceState)
    {
        finalBarriers[finalBarrierCount++] = {
            D3D12_RESOURCE_BARRIER_TYPE_TRANSITION, D3D12_RESOURCE_BARRIER_FLAG_NONE,
            {targetResource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, targetState,
             targetInfo.fResourceState}};
    }
    if (finalBarrierCount != 0)
    {
        commandList->ResourceBarrier(finalBarrierCount, finalBarriers.data());
    }
    if (FAILED(commandList->Close()))
    {
        return impl_->reportFailure("close_command_list_failed");
    }
    ID3D12CommandList* commandLists[] = {commandList};
    impl_->state.queue->ExecuteCommandLists(1, commandLists);
    if (!signalFrame(impl_->state, *commandFrame))
    {
        return impl_->reportFailure("signal_frame_failed");
    }
    impl_->reportSuccess(frame, submittedDrawCalls, uploadedMeshCount,
                         targetInfo.fResourceState);
    return true;
}

void DomDownToolPreview::Dx12GisRenderer::Clear()
{
    if (impl_)
    {
        impl_->uploader.cancelPending();
        impl_->state.waitForAllFrames();
        impl_->state.gpuMeshes.clear();
        impl_->state.requestedMeshes.clear();
        impl_->state.lastReadyBatches.clear();
        ++impl_->state.uploadGeneration;
        impl_->state.stats = {};
        impl_->lastFailure.clear();
        impl_->successLogged = false;
    }
}

DomDownToolPreview::Dx12GisRenderStats DomDownToolPreview::Dx12GisRenderer::stats() const
{
    return impl_ ? impl_->state.stats : Dx12GisRenderStats{};
}
