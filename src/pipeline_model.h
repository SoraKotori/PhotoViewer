#pragma once

#include "catalog.h"
#include "navigation.h"
#include "pipeline_types.h"
#include "reservation_planner.h"

#include <string_view>

namespace pv {

class PipelineModel;

struct CatalogRemap {
    std::size_t destination = 0;
    std::uint64_t generation = 0;
    SlotId compressed_slot = kInvalidSlot;
    SlotId staging_slot = kInvalidSlot;
};

class StorageFrameAccess {
public:
    [[nodiscard]] const ImageRecord& View(const std::size_t index) const {
        return frames_->at(index);
    }
    [[nodiscard]] IoRequest* Io(const std::size_t index) const {
        return frames_->at(index).Io();
    }
    void MarkFailed(const std::size_t index) const {
        frames_->at(index).MarkFailed();
    }
    void AttachIo(const std::size_t index, IoRequest* request) const {
        frames_->at(index).AttachIo(request);
    }
    void DetachIo(const std::size_t index, IoRequest* request) const {
        frames_->at(index).DetachIo(request);
    }
    void AttachCompressedSlot(const std::size_t index, const SlotId id) const {
        frames_->at(index).AttachCompressedSlot(id);
    }
    void ClearCompressedSlot(const std::size_t index, const SlotId id) const {
        frames_->at(index).ClearCompressedSlot(id);
    }

private:
    friend class PipelineModel;
    explicit StorageFrameAccess(std::vector<ImageRecord>& frames) noexcept
        : frames_(&frames) {}
    std::vector<ImageRecord>* frames_;
};

class DecodeFrameAccess {
public:
    [[nodiscard]] const ImageRecord& View(const std::size_t index) const {
        return frames_->at(index);
    }
    void MarkFailed(const std::size_t index) const {
        frames_->at(index).MarkFailed();
    }
    void BeginWork(const std::size_t index) const {
        frames_->at(index).BeginWork();
    }
    void EndWork(const std::size_t index) const {
        frames_->at(index).EndWork();
    }
    void AttachStagingSlot(const std::size_t index, const SlotId id) const {
        frames_->at(index).AttachStagingSlot(id);
    }
    void ClearStagingSlot(const std::size_t index, const SlotId id) const {
        frames_->at(index).ClearStagingSlot(id);
    }
    void ClearCompressedSlot(const std::size_t index, const SlotId id) const {
        frames_->at(index).ClearCompressedSlot(id);
    }

private:
    friend class PipelineModel;
    explicit DecodeFrameAccess(std::vector<ImageRecord>& frames) noexcept
        : frames_(&frames) {}
    std::vector<ImageRecord>* frames_;
};

class GraphicsFrameAccess {
public:
    [[nodiscard]] const ImageRecord& View(const std::size_t index) const {
        return frames_->at(index);
    }
    void MarkFailed(const std::size_t index) const {
        frames_->at(index).MarkFailed();
    }
    void ClearStagingSlot(const std::size_t index, const SlotId id) const {
        frames_->at(index).ClearStagingSlot(id);
    }

private:
    friend class PipelineModel;
    explicit GraphicsFrameAccess(std::vector<ImageRecord>& frames) noexcept
        : frames_(&frames) {}
    std::vector<ImageRecord>* frames_;
};

// Narrow write capabilities keep executor-specific catalog and presentation
// mutations from turning a read-only model dependency into a global backdoor.
class StorageCatalogAccess {
public:
    void RecordHeader(std::size_t index, const PngInfo& png) const;
    void RecordFileSize(std::size_t index, std::uint64_t bytes) const;
    void MarkReservationPlanDirty() const noexcept;

private:
    friend class PipelineModel;
    explicit StorageCatalogAccess(PipelineModel& model) noexcept
        : model_(&model) {}
    PipelineModel* model_;
};

class PresentationCompletionAccess {
public:
    void Complete(std::size_t index) const;

private:
    friend class PipelineModel;
    explicit PresentationCompletionAccess(PipelineModel& model) noexcept
        : model_(&model) {}
    PipelineModel* model_;
};

// Owns catalog identity, per-frame control records, navigation intent and the
// reservation plan. Catalog replacement and navigation changes cannot bypass
// generation/reset/dirty invariants.
class PipelineModel {
public:
    explicit PipelineModel(const PipelineLimits& limits);

    void LoadInitial(Catalog catalog);
    [[nodiscard]] CatalogRemap MergeCompletedCatalog(Catalog catalog);
    void Navigate(int direction, bool repeat);
    void ApplyNavigationSequence(std::wstring_view steps);
    void ReleaseNavigation(int direction);
    void CompletePresentation(std::size_t index);
    void MarkReservationPlanDirty() noexcept;
    void RecordHeader(std::size_t index, const PngInfo& png);
    void RecordFileSize(std::size_t index, std::uint64_t bytes);

    [[nodiscard]] const Catalog& CatalogData() const noexcept { return catalog_; }
    [[nodiscard]] std::size_t FrameCount() const noexcept { return frames_.size(); }
    [[nodiscard]] const CatalogItem& CatalogItemAt(std::size_t index) const;
    [[nodiscard]] const ImageRecord& FrameView(std::size_t index) const;
    [[nodiscard]] const std::vector<ImageRecord>& Frames() const noexcept {
        return frames_;
    }
    [[nodiscard]] const NavigationState& NavigationView() const noexcept {
        return navigation_;
    }
    [[nodiscard]] const ReservationPlanner& ReservationPlan() const noexcept {
        return reservations_;
    }
    [[nodiscard]] std::uint64_t Generation() const noexcept {
        return generation_;
    }

private:
    friend class PipelineRuntime;
    friend class PipelineScheduler;
    [[nodiscard]] ImageRecord& FrameAt(std::size_t index);
    [[nodiscard]] std::vector<ImageRecord>& Frames() noexcept { return frames_; }
    [[nodiscard]] ReservationPlanner& Reservations() noexcept {
        return reservations_;
    }
    [[nodiscard]] const ReservationPlanner& Reservations() const noexcept {
        return reservations_;
    }
    [[nodiscard]] StorageFrameAccess StorageFrames() noexcept {
        return StorageFrameAccess(frames_);
    }
    [[nodiscard]] DecodeFrameAccess DecodeFrames() noexcept {
        return DecodeFrameAccess(frames_);
    }
    [[nodiscard]] GraphicsFrameAccess GraphicsFrames() noexcept {
        return GraphicsFrameAccess(frames_);
    }
    [[nodiscard]] StorageCatalogAccess StorageCatalog() noexcept {
        return StorageCatalogAccess(*this);
    }
    [[nodiscard]] PresentationCompletionAccess PresentationCompletion()
        noexcept {
        return PresentationCompletionAccess(*this);
    }
    void ResetFrames();

    Catalog catalog_;
    NavigationState navigation_;
    std::vector<ImageRecord> frames_;
    std::uint64_t generation_ = 1;
    ReservationPlanner reservations_;
};

inline void StorageCatalogAccess::RecordHeader(
    const std::size_t index, const PngInfo& png) const {
    model_->RecordHeader(index, png);
}

inline void StorageCatalogAccess::RecordFileSize(
    const std::size_t index, const std::uint64_t bytes) const {
    model_->RecordFileSize(index, bytes);
}

inline void StorageCatalogAccess::MarkReservationPlanDirty() const noexcept {
    model_->MarkReservationPlanDirty();
}

inline void PresentationCompletionAccess::Complete(
    const std::size_t index) const {
    model_->CompletePresentation(index);
}

}  // namespace pv
