#include "pipeline_types.h"

#include <stdexcept>

namespace pv {

void ImageRecord::AttachIo(IoRequest* const request) {
    if (!request || io_) throw std::logic_error("invalid I/O attachment");
    io_ = request;
}

void ImageRecord::DetachIo(IoRequest* const request) {
    if (!request || io_ != request) throw std::logic_error("invalid I/O detachment");
    io_ = nullptr;
}

void ImageRecord::BeginWork() {
    if (work_active_) throw std::logic_error("decode work already active");
    work_active_ = true;
}

void ImageRecord::AssignCompressedReservation(const ReservationId id) {
    if (id == kInvalidReservation || compressed_reservation_ != kInvalidReservation) {
        throw std::logic_error("invalid compressed reservation assignment");
    }
    compressed_reservation_ = id;
}

void ImageRecord::AssignStagingReservation(const ReservationId id) {
    if (id == kInvalidReservation || staging_reservation_ != kInvalidReservation) {
        throw std::logic_error("invalid staging reservation assignment");
    }
    staging_reservation_ = id;
}

void ImageRecord::AssignGpuTextureReservation(const ReservationId id) {
    if (id == kInvalidReservation ||
        gpu_texture_reservation_ != kInvalidReservation) {
        throw std::logic_error("invalid GPU reservation assignment");
    }
    gpu_texture_reservation_ = id;
}

void ImageRecord::ClearCompressedReservation(const ReservationId id) {
    if (compressed_reservation_ != id) return;
    compressed_reservation_ = kInvalidReservation;
}

void ImageRecord::ClearStagingReservation(const ReservationId id) {
    if (staging_reservation_ != id) return;
    staging_reservation_ = kInvalidReservation;
}

void ImageRecord::ClearGpuTextureReservation(const ReservationId id) {
    if (gpu_texture_reservation_ != id) return;
    gpu_texture_reservation_ = kInvalidReservation;
}

void ImageRecord::AttachCompressedSlot(const SlotId id) {
    if (id == kInvalidSlot || compressed_slot_ != kInvalidSlot) {
        throw std::logic_error("invalid compressed slot attachment");
    }
    compressed_slot_ = id;
}

void ImageRecord::AttachStagingSlot(const SlotId id) {
    if (id == kInvalidSlot || staging_slot_ != kInvalidSlot) {
        throw std::logic_error("invalid staging slot attachment");
    }
    staging_slot_ = id;
}

void ImageRecord::ClearCompressedSlot(const SlotId id) {
    if (compressed_slot_ != id) return;
    compressed_slot_ = kInvalidSlot;
}

void ImageRecord::ClearStagingSlot(const SlotId id) {
    if (staging_slot_ != id) return;
    staging_slot_ = kInvalidSlot;
}

void ImageRecord::Initialize(const std::uint64_t generation,
                             const bool failed) noexcept {
    *this = {};
    generation_ = generation;
    failed_ = failed;
}

}  // namespace pv
