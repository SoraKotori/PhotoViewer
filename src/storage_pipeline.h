#pragma once

#include "iocp_transport.h"
#include "pipeline_limits.h"
#include "pipeline_model.h"
#include "pipeline_resources.h"

#include <span>
#include <vector>

namespace pv {

class RuntimeTelemetry;

// Owns the Windows file-I/O executor and the complete lifetime of each
// submitted read. The coordinator sees frame-level progress, never OVERLAPPED
// or completion-port details.
class StoragePipeline {
public:
    StoragePipeline(const PipelineLimits& limits, const PipelineModel& model,
                    StorageCatalogAccess catalog,
                    StorageFrameAccess frames,
                    const PipelineResources& resources,
                    StorageResourceAccess slots, RuntimeTelemetry& telemetry);
    ~StoragePipeline();

    StoragePipeline(const StoragePipeline&) = delete;
    StoragePipeline& operator=(const StoragePipeline&) = delete;

    [[nodiscard]] HANDLE CompletionEvent() const noexcept;
    [[nodiscard]] bool Enabled() const noexcept;
    [[nodiscard]] std::size_t TransferGranularity() const noexcept;
    [[nodiscard]] std::size_t CompressedAlignment() const noexcept;
    [[nodiscard]] bool InitialContentCompleted() const noexcept;

    [[nodiscard]] bool DrainCompletions();
    void SubmitEligibleReads();
    void RetireRead(std::size_t frame);
    void RemapActiveRead(std::size_t destination);
    void Shutdown();

    [[nodiscard]] std::span<const std::size_t> HeaderReadyFrames() const noexcept;
    void ClearHeaderReadyFrames() noexcept;

private:
    void OnCompletion(IoRequest* request, OVERLAPPED* overlapped,
                      DWORD result, ULONG_PTR transferred);
    void OnHeaderReady(IoRequest* request, DWORD result,
                       ULONG_PTR transferred);
    void OnContentReady(IoRequest* request, DWORD result,
                        ULONG_PTR transferred);
    void FinishRead(IoRequest* request);

    const PipelineLimits& limits_;
    const PipelineModel& model_;
    StorageCatalogAccess catalog_;
    StorageFrameAccess frames_;
    const PipelineResources& resources_;
    StorageResourceAccess slots_;
    RuntimeTelemetry& telemetry_;
    IocpTransport transport_;
    std::vector<std::size_t> header_ready_frames_;
    bool initial_content_completed_ = false;
    bool shutdown_ = false;
};

}  // namespace pv
