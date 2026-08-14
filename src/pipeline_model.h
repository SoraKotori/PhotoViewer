#pragma once

#include "catalog.h"
#include "linear_capability.h"
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

class StorageFrameAccess final
    : private LinearCapability<std::vector<ImageRecord>> {
public:
    StorageFrameAccess(const StorageFrameAccess&) = delete;
    StorageFrameAccess& operator=(const StorageFrameAccess&) = delete;
    StorageFrameAccess(StorageFrameAccess&&) noexcept = default;
    StorageFrameAccess& operator=(StorageFrameAccess&&) = delete;
    using LinearCapability::IsValid;

    [[nodiscard]] const ImageRecord& View(const std::size_t index) const {
        return Target().at(index);
    }
    [[nodiscard]] IoRequest* Io(const std::size_t index) const {
        return Target().at(index).Io();
    }
    void MarkFailed(const std::size_t index) {
        Target().at(index).MarkFailed();
    }
    void AttachIo(const std::size_t index, IoRequest* request) {
        Target().at(index).AttachIo(request);
    }
    void DetachIo(const std::size_t index, IoRequest* request) {
        Target().at(index).DetachIo(request);
    }
    void AttachCompressedSlot(const std::size_t index, const SlotId id) {
        Target().at(index).AttachCompressedSlot(id);
    }
    void ClearCompressedSlot(const std::size_t index, const SlotId id) {
        Target().at(index).ClearCompressedSlot(id);
    }

private:
    friend class PipelineModel;
    explicit StorageFrameAccess(std::vector<ImageRecord>& frames) noexcept
        : LinearCapability(frames) {}
};

class DecodeFrameAccess final
    : private LinearCapability<std::vector<ImageRecord>> {
public:
    DecodeFrameAccess(const DecodeFrameAccess&) = delete;
    DecodeFrameAccess& operator=(const DecodeFrameAccess&) = delete;
    DecodeFrameAccess(DecodeFrameAccess&&) noexcept = default;
    DecodeFrameAccess& operator=(DecodeFrameAccess&&) = delete;
    using LinearCapability::IsValid;

    [[nodiscard]] const ImageRecord& View(const std::size_t index) const {
        return Target().at(index);
    }
    void MarkFailed(const std::size_t index) {
        Target().at(index).MarkFailed();
    }
    void BeginWork(const std::size_t index) {
        Target().at(index).BeginWork();
    }
    void EndWork(const std::size_t index) {
        Target().at(index).EndWork();
    }
    void AttachStagingSlot(const std::size_t index, const SlotId id) {
        Target().at(index).AttachStagingSlot(id);
    }
    void ClearStagingSlot(const std::size_t index, const SlotId id) {
        Target().at(index).ClearStagingSlot(id);
    }
    void ClearCompressedSlot(const std::size_t index, const SlotId id) {
        Target().at(index).ClearCompressedSlot(id);
    }

private:
    friend class PipelineModel;
    explicit DecodeFrameAccess(std::vector<ImageRecord>& frames) noexcept
        : LinearCapability(frames) {}
};

class GraphicsFrameAccess final
    : private LinearCapability<std::vector<ImageRecord>> {
public:
    GraphicsFrameAccess(const GraphicsFrameAccess&) = delete;
    GraphicsFrameAccess& operator=(const GraphicsFrameAccess&) = delete;
    GraphicsFrameAccess(GraphicsFrameAccess&&) noexcept = default;
    GraphicsFrameAccess& operator=(GraphicsFrameAccess&&) = delete;
    using LinearCapability::IsValid;

    [[nodiscard]] const ImageRecord& View(const std::size_t index) const {
        return Target().at(index);
    }
    void MarkFailed(const std::size_t index) {
        Target().at(index).MarkFailed();
    }
    void ClearStagingSlot(const std::size_t index, const SlotId id) {
        Target().at(index).ClearStagingSlot(id);
    }

private:
    friend class PipelineModel;
    explicit GraphicsFrameAccess(std::vector<ImageRecord>& frames) noexcept
        : LinearCapability(frames) {}
};

// Narrow write capabilities keep executor-specific catalog and presentation
// mutations from turning a read-only model dependency into a global backdoor.
class StorageCatalogAccess final : private LinearCapability<PipelineModel> {
public:
    StorageCatalogAccess(const StorageCatalogAccess&) = delete;
    StorageCatalogAccess& operator=(const StorageCatalogAccess&) = delete;
    StorageCatalogAccess(StorageCatalogAccess&&) noexcept = default;
    StorageCatalogAccess& operator=(StorageCatalogAccess&&) = delete;
    using LinearCapability::IsValid;

    void RecordResourcePlan(std::size_t index,
                            const PngResourcePlan& plan);
    void RecordFileSize(std::size_t index, std::uint64_t bytes);
    void MarkReservationPlanDirty() noexcept;

private:
    friend class PipelineModel;
    explicit StorageCatalogAccess(PipelineModel& model) noexcept
        : LinearCapability(model) {}
};

class PresentationCompletionAccess final
    : private LinearCapability<PipelineModel> {
public:
    PresentationCompletionAccess(const PresentationCompletionAccess&) = delete;
    PresentationCompletionAccess& operator=(
        const PresentationCompletionAccess&) = delete;
    PresentationCompletionAccess(PresentationCompletionAccess&&) noexcept =
        default;
    PresentationCompletionAccess& operator=(PresentationCompletionAccess&&) =
        delete;
    using LinearCapability::IsValid;

    void Complete(std::size_t index);

private:
    friend class PipelineModel;
    explicit PresentationCompletionAccess(PipelineModel& model) noexcept
        : LinearCapability(model) {}
};

// Grants the scheduler only reservation planning and the frame transitions it
// coordinates. It cannot replace catalogs, navigate, or obtain mutable frame
// storage.
class SchedulerModelAccess final : private LinearCapability<PipelineModel> {
public:
    SchedulerModelAccess(const SchedulerModelAccess&) = delete;
    SchedulerModelAccess& operator=(const SchedulerModelAccess&) = delete;
    SchedulerModelAccess(SchedulerModelAccess&&) noexcept = default;
    SchedulerModelAccess& operator=(SchedulerModelAccess&&) = delete;
    using LinearCapability::IsValid;

    [[nodiscard]] std::size_t FrameCount() const noexcept;
    [[nodiscard]] const std::vector<ImageRecord>& Frames() const;
    [[nodiscard]] const ImageRecord& Frame(std::size_t index) const;
    [[nodiscard]] const CatalogItem& CatalogItemAt(std::size_t index) const;
    [[nodiscard]] const NavigationState& Navigation() const;
    [[nodiscard]] ReservationPlanner& Reservations();
    [[nodiscard]] const ReservationPlanner& Reservations() const;

    void MarkFailed(std::size_t frame);
    void ClearFailure(std::size_t frame);
    void AssignCompressedReservation(std::size_t frame, ReservationId id);
    void ClearCompressedReservation(std::size_t frame, ReservationId id);
    void AssignStagingReservation(std::size_t frame, ReservationId id);
    void ClearStagingReservation(std::size_t frame, ReservationId id);
    void ClearStagingSlot(std::size_t frame, SlotId id);
    void AssignGpuTextureReservation(std::size_t frame, ReservationId id);
    void ClearGpuTextureReservation(std::size_t frame, ReservationId id);

private:
    friend class PipelineModel;
    explicit SchedulerModelAccess(PipelineModel& model) noexcept
        : LinearCapability(model) {}
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
    friend class StorageCatalogAccess;
    friend class PresentationCompletionAccess;
    friend class SchedulerModelAccess;
    void CompletePresentation(std::size_t index);
    void MarkReservationPlanDirty() noexcept;
    void RecordResourcePlan(std::size_t index,
                            const PngResourcePlan& plan);
    void RecordFileSize(std::size_t index, std::uint64_t bytes);
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
    [[nodiscard]] SchedulerModelAccess SchedulerAccess() noexcept {
        return SchedulerModelAccess(*this);
    }
    void ResetFrames();

    Catalog catalog_;
    NavigationState navigation_;
    std::vector<ImageRecord> frames_;
    std::uint64_t generation_ = 1;
    ReservationPlanner reservations_;
};

inline void StorageCatalogAccess::RecordResourcePlan(
    const std::size_t index, const PngResourcePlan& plan) {
    Target().RecordResourcePlan(index, plan);
}

inline void StorageCatalogAccess::RecordFileSize(
    const std::size_t index, const std::uint64_t bytes) {
    Target().RecordFileSize(index, bytes);
}

inline void StorageCatalogAccess::MarkReservationPlanDirty() noexcept {
    Target().MarkReservationPlanDirty();
}

inline void PresentationCompletionAccess::Complete(
    const std::size_t index) {
    Target().CompletePresentation(index);
}

inline std::size_t SchedulerModelAccess::FrameCount() const noexcept {
    return Target().FrameCount();
}

inline const std::vector<ImageRecord>& SchedulerModelAccess::Frames() const {
    return Target().Frames();
}

inline const ImageRecord& SchedulerModelAccess::Frame(
    const std::size_t index) const {
    return Target().FrameView(index);
}

inline const CatalogItem& SchedulerModelAccess::CatalogItemAt(
    const std::size_t index) const {
    return Target().CatalogItemAt(index);
}

inline const NavigationState& SchedulerModelAccess::Navigation() const {
    return Target().NavigationView();
}

inline ReservationPlanner& SchedulerModelAccess::Reservations() {
    return Target().Reservations();
}

inline const ReservationPlanner& SchedulerModelAccess::Reservations() const {
    return Target().Reservations();
}

inline void SchedulerModelAccess::MarkFailed(const std::size_t frame) {
    Target().FrameAt(frame).MarkFailed();
}

inline void SchedulerModelAccess::ClearFailure(const std::size_t frame) {
    Target().FrameAt(frame).ClearFailure();
}

inline void SchedulerModelAccess::AssignCompressedReservation(
    const std::size_t frame, const ReservationId id) {
    Target().FrameAt(frame).AssignCompressedReservation(id);
}

inline void SchedulerModelAccess::ClearCompressedReservation(
    const std::size_t frame, const ReservationId id) {
    Target().FrameAt(frame).ClearCompressedReservation(id);
}

inline void SchedulerModelAccess::AssignStagingReservation(
    const std::size_t frame, const ReservationId id) {
    Target().FrameAt(frame).AssignStagingReservation(id);
}

inline void SchedulerModelAccess::ClearStagingReservation(
    const std::size_t frame, const ReservationId id) {
    Target().FrameAt(frame).ClearStagingReservation(id);
}

inline void SchedulerModelAccess::ClearStagingSlot(
    const std::size_t frame, const SlotId id) {
    Target().FrameAt(frame).ClearStagingSlot(id);
}

inline void SchedulerModelAccess::AssignGpuTextureReservation(
    const std::size_t frame, const ReservationId id) {
    Target().FrameAt(frame).AssignGpuTextureReservation(id);
}

inline void SchedulerModelAccess::ClearGpuTextureReservation(
    const std::size_t frame, const ReservationId id) {
    Target().FrameAt(frame).ClearGpuTextureReservation(id);
}

}  // namespace pv
