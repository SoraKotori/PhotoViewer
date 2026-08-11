#include "pipeline_runtime.h"

#include "common.h"

namespace pv {

PipelineRuntime::PipelineRuntime(PipelineObserver& observer, Config& config,
                                 ValidationState& validation,
                                 ViewerWindow& window,
                                 bool& catalog_loading)
    : observer_(observer),
      config_(config),
      validation_(validation),
      window_(window),
      catalog_loading_(catalog_loading),
      completion_queue_(config.staging_slot_count),
      completion_batch_(config.staging_slot_count),
      state_(config) {
    io_completion_port_.Reset(CreateIoCompletionPort(
        INVALID_HANDLE_VALUE, nullptr, 0, 1));
    if (!io_completion_port_) ThrowLastError("CreateIoCompletionPort");
    io_completion_event_.Reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!io_completion_event_) ThrowLastError("CreateEventW(IOCP)");
}

PipelineRuntime::~PipelineRuntime() {
    decoders_.reset();
    if (graphics_device_ready_) {
        for (SlotId id = 0; id < state_.slots.StagingCount(); ++id) {
            graphics_.UnmapDecodeStaging(state_.slots.StagingAt(id).resource);
        }
    }
    CancelAllIo();
}

}  // namespace pv
