#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

namespace pv {

using SlotId = std::uint32_t;
constexpr SlotId kInvalidSlot = std::numeric_limits<SlotId>::max();
constexpr std::size_t kInvalidFrame = std::numeric_limits<std::size_t>::max();
using ReservationId = std::uint32_t;
constexpr ReservationId kInvalidReservation =
    std::numeric_limits<ReservationId>::max();

enum class PipelineStage {
    Outside,
    WaitingIo,
    IoInFlight,
    CompressedReady,
    DecodeQueued,
    DecodedStagingAvailable,
    Uploading,
    PresentationTextureAvailable,
    CancelPending,
    Failed,
};

struct IoRequest;
class StorageFrameAccess;
class DecodeFrameAccess;
class GraphicsFrameAccess;
class PipelineScheduler;

class ImageRecord {
public:
    [[nodiscard]] bool Failed() const noexcept { return failed_; }
    [[nodiscard]] std::uint64_t Generation() const noexcept {
        return generation_;
    }
    [[nodiscard]] bool IoActive() const noexcept { return io_ != nullptr; }
    [[nodiscard]] bool WorkActive() const noexcept { return work_active_; }
    [[nodiscard]] ReservationId CompressedReservation() const noexcept {
        return compressed_reservation_;
    }
    [[nodiscard]] ReservationId StagingReservation() const noexcept {
        return staging_reservation_;
    }
    [[nodiscard]] ReservationId GpuTextureReservation() const noexcept {
        return gpu_texture_reservation_;
    }
    [[nodiscard]] SlotId CompressedSlot() const noexcept {
        return compressed_slot_;
    }
    [[nodiscard]] SlotId StagingSlot() const noexcept { return staging_slot_; }
    [[nodiscard]] static ImageRecord FailedRecord() noexcept {
        ImageRecord record;
        record.failed_ = true;
        return record;
    }

private:
    friend class PipelineModel;
    friend class PipelineRuntime;
    friend class PipelineScheduler;
    friend class StorageFrameAccess;
    friend class DecodeFrameAccess;
    friend class GraphicsFrameAccess;
    void MarkFailed() noexcept { failed_ = true; }
    void ClearFailure() noexcept { failed_ = false; }
    void BeginWork();
    void EndWork() noexcept { work_active_ = false; }
    void AssignCompressedReservation(ReservationId id);
    void AssignStagingReservation(ReservationId id);
    void AssignGpuTextureReservation(ReservationId id);
    void ClearCompressedReservation(ReservationId id);
    void ClearStagingReservation(ReservationId id);
    void ClearGpuTextureReservation(ReservationId id);
    void AttachCompressedSlot(SlotId id);
    void AttachStagingSlot(SlotId id);
    void ClearCompressedSlot(SlotId id);
    void ClearStagingSlot(SlotId id);
    [[nodiscard]] IoRequest* Io() const noexcept { return io_; }
    void AttachIo(IoRequest* request);
    void DetachIo(IoRequest* request);
    void Initialize(std::uint64_t generation, bool failed) noexcept;

    bool failed_ = false;
    std::uint64_t generation_ = 0;
    IoRequest* io_ = nullptr;
    bool work_active_ = false;
    ReservationId compressed_reservation_ = kInvalidReservation;
    ReservationId staging_reservation_ = kInvalidReservation;
    ReservationId gpu_texture_reservation_ = kInvalidReservation;
    SlotId compressed_slot_ = kInvalidSlot;
    SlotId staging_slot_ = kInvalidSlot;
};

struct DecodeWork {
    std::size_t index = 0;
    std::uint64_t generation = 0;
    SlotId compressed_slot = kInvalidSlot;
    SlotId staging_slot = kInvalidSlot;
};

struct DecodeResult {
    std::size_t index = 0;
    std::uint64_t generation = 0;
    bool success = false;
    std::int32_t error = 0;
    SlotId staging_slot = kInvalidSlot;
};

struct ReleasedInput {
    std::size_t index = 0;
    std::uint64_t generation = 0;
    SlotId compressed_slot = kInvalidSlot;
};

struct UploadTicket {
    std::size_t index = 0;
    std::uint64_t generation = 0;
    std::uint64_t fence_value = 0;
    SlotId staging_slot = kInvalidSlot;
    SlotId gpu_texture_slot = kInvalidSlot;
    std::size_t bytes = 0;
};

}  // namespace pv
