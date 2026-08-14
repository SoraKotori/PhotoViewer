#include "pipeline_runtime.h"

#include "win32_support.h"

namespace pv {

PipelineRuntime::PipelineRuntime(PipelineObserver& observer, const Config& config,
                                 ViewerWindow& window,
                                 RuntimeTelemetry* const telemetry)
    : limits_(config),
      window_(window),
      telemetry_(telemetry ? *telemetry : disabled_telemetry_),
      model_(limits_),
      resources_(limits_),
      storage_(limits_, model_, model_.StorageCatalog(), model_.StorageFrames(),
               resources_,
               resources_.BackingAbandonment(),
               resources_.StorageAccess(), telemetry_),
      graphics_(limits_, model_, model_.PresentationCompletion(),
                model_.GraphicsFrames(), resources_,
                resources_.GraphicsAccess(), observer, window_),
      decode_(limits_, model_, model_.DecodeFrames(), resources_,
              resources_.DecodeAccess(), graphics_, telemetry_,
              config.png_validation),
      scheduler_(limits_, model_.SchedulerAccess(), resources_.SchedulerAccess(),
                 storage_, decode_, graphics_, telemetry_) {}

PipelineRuntime::~PipelineRuntime() noexcept {
    decode_.Stop();
    graphics_.DrainForShutdown();
    graphics_.UnmapAllStagingForShutdown();
    storage_.Shutdown();
}

void PipelineRuntime::StartWorkers(const std::size_t worker_count) {
    decode_.Start(worker_count);
    telemetry_.Mark(StartupMilestone::DecodersReady);
}

void PipelineRuntime::InitializeGraphics() {
    graphics_.InitializeDevice();
    decode_.EnableGraphicsDevice();
    telemetry_.Mark(StartupMilestone::GraphicsDeviceReady);

    (void)scheduler_.DrainStorage();
    // Storage continues in the kernel during synchronous device creation.
    // Reconcile each completed phase before exposing the next GPU capability.
    scheduler_.Pump();
    graphics_.Initialize2D();
    if (scheduler_.DrainStorage()) scheduler_.Pump();
    graphics_.InitializeSwapChain();
    if (scheduler_.DrainStorage()) scheduler_.Pump();
    graphics_.InitializeBackBufferTarget();
    (void)scheduler_.DrainStorage();
    scheduler_.Pump();
    telemetry_.Mark(StartupMilestone::GraphicsReady);
}

HANDLE PipelineRuntime::IoCompletionEvent() const noexcept {
    return storage_.CompletionEvent();
}

HANDLE PipelineRuntime::WorkerCompletionEvent() const noexcept {
    return decode_.CompletionEvent();
}

HANDLE PipelineRuntime::FrameWaitEvent() const noexcept {
    return graphics_.FrameWaitEvent();
}

HANDLE PipelineRuntime::GpuWaitEvent() const noexcept {
    return graphics_.GpuWaitEvent();
}

void PipelineRuntime::OnIoReady() {
    if (scheduler_.DrainStorage()) scheduler_.Pump();
}

void PipelineRuntime::OnWorkerNotification() {
    if (decode_.HandleCompletions()) scheduler_.Pump();
}

void PipelineRuntime::OnFrameCreditAvailable() {
    graphics_.GrantFrameCredit();
    scheduler_.Pump();
}

void PipelineRuntime::OnGpuReady() {
    if (graphics_.HandleGpuCompletion()) scheduler_.Pump();
}

void PipelineRuntime::ResizeSurface(const UINT width, const UINT height) {
    if (graphics_.Ready()) {
        graphics_.Resize(width, height);
        scheduler_.Pump();
    }
}

void PipelineRuntime::Paint() {
    graphics_.Paint();
    if (graphics_.Ready()) scheduler_.Pump();
}

void PipelineRuntime::LoadInitialCatalog(Catalog catalog,
                                         const bool catalog_complete) {
    scheduler_.SetCatalogComplete(catalog_complete);
    model_.LoadInitial(std::move(catalog));
    scheduler_.InitializeReservations();
    graphics_.RequestRedraw(false);
    scheduler_.Pump();
    telemetry_.Mark(StartupMilestone::InitialIoSubmitted);
}

void PipelineRuntime::CompleteCatalog(Catalog catalog) {
    if (scheduler_.CatalogComplete()) {
        throw std::logic_error("catalog already complete");
    }
    const CatalogRemap remap =
        model_.MergeCompletedCatalog(std::move(catalog));
    decode_.Remap(0, remap.destination, remap.generation);
    storage_.RemapActiveRead(remap.destination);
    if (remap.compressed_slot != kInvalidSlot) {
        resources_.Slots().RemapCompressedImage(remap.compressed_slot,
                                              remap.destination);
    }
    if (remap.staging_slot != kInvalidSlot) {
        resources_.Slots().RemapStagingImage(remap.staging_slot,
                                           remap.destination);
    }
    scheduler_.InitializeReservations();
    graphics_.RequestRedraw(false);
    scheduler_.SetCatalogComplete(true);
    scheduler_.Pump();
}

void PipelineRuntime::Navigate(const int direction, const bool repeat) {
    model_.Navigate(direction, repeat);
    scheduler_.Pump();
}

void PipelineRuntime::ApplyNavigationSequence(const std::wstring_view steps) {
    model_.ApplyNavigationSequence(steps);
    scheduler_.Pump();
}

void PipelineRuntime::ReleaseNavigation(const int direction) {
    model_.ReleaseNavigation(direction);
    scheduler_.Pump();
}

std::size_t PipelineRuntime::FrameCount() const noexcept {
    return model_.Frames().size();
}

std::size_t PipelineRuntime::CurrentIndex() const noexcept {
    return model_.NavigationView().CurrentIndex();
}

bool PipelineRuntime::InitialContentPending() const noexcept {
    if (model_.Frames().empty()) return false;
    const std::size_t initial = model_.NavigationView().CurrentIndex();
    if (initial >= model_.Frames().size() || model_.Frames()[initial].Failed()) {
        return false;
    }
    return !storage_.InitialContentCompleted();
}

bool PipelineRuntime::InitialContentFailed() const noexcept {
    if (model_.Frames().empty()) return false;
    const std::size_t initial = model_.NavigationView().CurrentIndex();
    return initial < model_.Frames().size() && model_.Frames()[initial].Failed();
}

bool PipelineRuntime::NavigationIdle() const noexcept {
    return model_.NavigationView().Empty();
}

const std::filesystem::path& PipelineRuntime::PathFor(
    const std::size_t index) const {
    return model_.CatalogItemAt(index).path;
}

std::optional<PipelineStage> PipelineRuntime::PendingFrameStage()
    const noexcept {
    const auto next = model_.NavigationView().NextIndex();
    if (!next || *next >= model_.Frames().size()) return std::nullopt;
    return scheduler_.StageOf(*next);
}

void PipelineRuntime::ResetPerformanceCounters() noexcept {
    decode_.ResetMetrics();
    graphics_.ResetMetrics();
}

}  // namespace pv
