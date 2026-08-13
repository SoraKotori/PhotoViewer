#pragma once

#include "catalog.h"
#include "config.h"
#include "decode_pipeline.h"
#include "graphics_pipeline.h"
#include "pipeline_model.h"
#include "pipeline_observer.h"
#include "pipeline_resources.h"
#include "pipeline_scheduler.h"
#include "pipeline_limits.h"
#include "runtime_telemetry.h"
#include "storage_pipeline.h"
#include "viewer_window.h"

#include <iosfwd>
#include <optional>
#include <string_view>

namespace pv {

// Main-thread coordinator for global control state. Storage, CPU decode and
// graphics executors own their internal queues and completion lifetimes.
class PipelineRuntime {
public:
    PipelineRuntime(PipelineObserver& observer, const Config& config,
                    ViewerWindow& window,
                    RuntimeTelemetry* telemetry = nullptr);
    ~PipelineRuntime() noexcept;

    PipelineRuntime(const PipelineRuntime&) = delete;
    PipelineRuntime& operator=(const PipelineRuntime&) = delete;

    void StartWorkers(std::size_t worker_count);
    void InitializeGraphics();

    [[nodiscard]] HANDLE IoCompletionEvent() const noexcept;
    [[nodiscard]] HANDLE WorkerCompletionEvent() const noexcept;
    [[nodiscard]] HANDLE FrameWaitEvent() const noexcept;
    [[nodiscard]] HANDLE GpuWaitEvent() const noexcept;
    void OnIoReady();
    void OnWorkerNotification();
    void OnFrameCreditAvailable();
    void OnGpuReady();
    void ResizeSurface(UINT width, UINT height);
    void Paint();

    void LoadInitialCatalog(Catalog catalog, bool catalog_complete);
    void CompleteCatalog(Catalog catalog);
    void Navigate(int direction, bool repeat);
    void ApplyNavigationSequence(std::wstring_view steps);
    void ReleaseNavigation(int direction);

    [[nodiscard]] std::size_t FrameCount() const noexcept;
    [[nodiscard]] std::size_t CurrentIndex() const noexcept;
    [[nodiscard]] bool InitialContentPending() const noexcept;
    [[nodiscard]] bool InitialContentFailed() const noexcept;
    [[nodiscard]] bool NavigationIdle() const noexcept;
    [[nodiscard]] const std::filesystem::path& PathFor(
        std::size_t index) const;
    [[nodiscard]] std::optional<PipelineStage> PendingFrameStage()
        const noexcept;

    void ResetPerformanceCounters() noexcept;
    void WriteDiagnostics(std::ostream& output) const;

private:
    const PipelineLimits limits_;
    ViewerWindow& window_;
    RuntimeTelemetry disabled_telemetry_;
    RuntimeTelemetry& telemetry_;
    PipelineModel model_;
    PipelineResources resources_;
    StoragePipeline storage_;
    GraphicsPipeline graphics_;
    DecodePipeline decode_;
    PipelineScheduler scheduler_;
};

}  // namespace pv
