#pragma once

#include "resource_slots.h"
#include "reservation.h"

namespace pv {

struct ImageRecord {
    bool failed = false;
    std::uint64_t generation = 0;
    IoRequest* io = nullptr;
    bool work_active = false;
    ReservationId compressed_reservation = kInvalidReservation;
    ReservationId staging_reservation = kInvalidReservation;
    ReservationId gpu_texture_reservation = kInvalidReservation;
    SlotId compressed_slot = kInvalidSlot;
    SlotId staging_slot = kInvalidSlot;
};

}  // namespace pv
