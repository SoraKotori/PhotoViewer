#include "pipeline_model.h"

#include <stdexcept>

namespace pv {

PipelineModel::PipelineModel(const PipelineLimits& limits)
    : reservations_(limits) {}

void PipelineModel::LoadInitial(Catalog catalog) {
    catalog_ = std::move(catalog);
    ++generation_;
    ResetFrames();
    navigation_.Reset(catalog_.initial_index, catalog_.items.size());
}

CatalogRemap PipelineModel::MergeCompletedCatalog(Catalog catalog) {
    if (catalog.items.empty() || catalog_.items.empty() || frames_.empty()) {
        throw std::runtime_error("asynchronous catalog returned no images");
    }

    const std::size_t initial = catalog.initial_index;
    catalog.items.at(initial) = std::move(catalog_.items.front());
    ImageRecord initial_frame = std::move(frames_.front());
    initial_frame.ClearCompressedReservation(
        initial_frame.CompressedReservation());
    initial_frame.ClearStagingReservation(initial_frame.StagingReservation());
    initial_frame.ClearGpuTextureReservation(
        initial_frame.GpuTextureReservation());

    catalog_ = std::move(catalog);
    ResetFrames();
    frames_.at(initial) = std::move(initial_frame);
    navigation_.Reset(initial, frames_.size());
    reservations_.MarkDirty();

    const ImageRecord& remapped = frames_[initial];
    return CatalogRemap{initial, generation_, remapped.CompressedSlot(),
                        remapped.StagingSlot()};
}

void PipelineModel::Navigate(const int direction, const bool repeat) {
    if (frames_.empty()) return;
    navigation_.Step(direction, repeat);
    reservations_.MarkDirty();
}

void PipelineModel::ApplyNavigationSequence(const std::wstring_view steps) {
    if (frames_.empty()) return;
    for (const wchar_t step : steps) {
        const int direction = step == L'L' ? -1 : 1;
        navigation_.Step(direction, false);
        navigation_.Release(direction);
    }
    reservations_.MarkDirty();
}

void PipelineModel::ReleaseNavigation(const int direction) {
    if (frames_.empty()) return;
    navigation_.Release(direction);
    reservations_.MarkDirty();
}

void PipelineModel::CompletePresentation(const std::size_t index) {
    navigation_.CompletePresentation(index);
    reservations_.MarkDirty();
}

void PipelineModel::MarkReservationPlanDirty() noexcept {
    reservations_.MarkDirty();
}

void PipelineModel::RecordResourcePlan(const std::size_t index,
                                       const PngResourcePlan& plan) {
    CatalogItem& item = catalog_.items.at(index);
    item.resource_plan = plan;
    item.header_valid = true;
    reservations_.MarkDirty();
}

void PipelineModel::RecordFileSize(const std::size_t index,
                                   const std::uint64_t bytes) {
    CatalogItem& item = catalog_.items.at(index);
    item.file_bytes = bytes;
    item.file_size_known = true;
    reservations_.MarkDirty();
}

const CatalogItem& PipelineModel::CatalogItemAt(const std::size_t index) const {
    return catalog_.items.at(index);
}

ImageRecord& PipelineModel::FrameAt(const std::size_t index) {
    return frames_.at(index);
}

const ImageRecord& PipelineModel::FrameView(const std::size_t index) const {
    return frames_.at(index);
}

void PipelineModel::ResetFrames() {
    frames_.assign(catalog_.items.size(), {});
    for (std::size_t index = 0; index < frames_.size(); ++index) {
        const CatalogItem& item = catalog_.items[index];
        frames_[index].Initialize(
            generation_, item.file_size_known && item.file_bytes == 0);
    }
}

}  // namespace pv
