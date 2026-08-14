#include "storage_pipeline.h"

#include <numeric>
#include <utility>

namespace pv {

StoragePipeline::StoragePipeline(const PipelineLimits& limits,
                                 const PipelineModel& model,
                                 StorageCatalogAccess catalog,
                                 StorageFrameAccess frames,
                                 const PipelineResources& resources,
                                 ResourceBackingAbandonment backing,
                                 StorageResourceAccess slots,
                                 RuntimeTelemetry& telemetry)
    : limits_(limits), model_(model), catalog_(std::move(catalog)),
      frames_(std::move(frames)), resources_(resources),
      backing_(std::move(backing)), slots_(std::move(slots)),
      telemetry_(telemetry) {
    header_ready_frames_.reserve(limits.compressed_slot_count);
}

StoragePipeline::~StoragePipeline() noexcept {
    if (!shutdown_) Shutdown();
}

HANDLE StoragePipeline::CompletionEvent() const noexcept {
    return transport_.CompletionEvent();
}

bool StoragePipeline::Enabled() const noexcept { return transport_.Enabled(); }

std::size_t StoragePipeline::TransferGranularity() const noexcept {
    return transport_.CachedTransferGranularity();
}

std::size_t StoragePipeline::CompressedAlignment() const noexcept {
    const std::size_t transfer = transport_.CachedTransferGranularity();
    return transfer == 0 ? 0 : std::lcm<std::size_t>(4096, transfer);
}

bool StoragePipeline::InitialContentCompleted() const noexcept {
    return initial_content_completed_;
}

std::span<const std::size_t> StoragePipeline::HeaderReadyFrames() const noexcept {
    return header_ready_frames_;
}

void StoragePipeline::ClearHeaderReadyFrames() noexcept {
    header_ready_frames_.clear();
}

}  // namespace pv
