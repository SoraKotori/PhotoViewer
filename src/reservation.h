#pragma once

#include "model.h"

namespace pv {

using ReservationId = std::uint32_t;
constexpr ReservationId kInvalidReservation =
    std::numeric_limits<ReservationId>::max();

struct ReservationEntry {
    std::size_t frame = kInvalidFrame;
    bool retiring = false;
};

class ReservationTable {
public:
    void Reset(const std::size_t capacity) {
        entries_.assign(capacity, {});
    }

    [[nodiscard]] std::size_t Capacity() const noexcept {
        return entries_.size();
    }

    [[nodiscard]] const ReservationEntry& At(const ReservationId id) const {
        return entries_.at(id);
    }

    [[nodiscard]] ReservationEntry& At(const ReservationId id) {
        return entries_.at(id);
    }

    [[nodiscard]] ReservationId FindFrame(const std::size_t frame) const noexcept {
        for (std::size_t id = 0; id < entries_.size(); ++id) {
            if (entries_[id].frame == frame) return static_cast<ReservationId>(id);
        }
        return kInvalidReservation;
    }

    [[nodiscard]] bool IsActive(const ReservationId id) const noexcept {
        return id != kInvalidReservation && id < entries_.size() &&
               entries_[id].frame != kInvalidFrame && !entries_[id].retiring;
    }

    [[nodiscard]] std::size_t AssignedCount() const noexcept {
        return static_cast<std::size_t>(std::count_if(
            entries_.begin(), entries_.end(),
            [](const ReservationEntry& entry) {
                return entry.frame != kInvalidFrame;
            }));
    }

    template <typename CanRelease, typename OnRelease, typename OnAssign,
              typename SelectFree>
    void Reconcile(const std::span<const std::size_t> desired,
                   CanRelease&& can_release, OnRelease&& on_release,
                   OnAssign&& on_assign, SelectFree&& select_free) {
        const std::size_t desired_count = std::min(desired.size(), entries_.size());
        const auto wanted = [&](const std::size_t frame) {
            return std::find(desired.begin(), desired.begin() + desired_count,
                             frame) != desired.begin() + desired_count;
        };

        for (ReservationId id = 0; id < entries_.size(); ++id) {
            ReservationEntry& entry = entries_[id];
            if (entry.frame == kInvalidFrame) continue;
            if (wanted(entry.frame)) {
                entry.retiring = false;
                continue;
            }
            if (!can_release(id, entry.frame)) {
                entry.retiring = true;
                continue;
            }
            on_release(id, entry.frame);
            entry = {};
        }

        for (std::size_t order = 0; order < desired_count; ++order) {
            const std::size_t frame = desired[order];
            if (FindFrame(frame) != kInvalidReservation) continue;
            const ReservationId id = select_free(frame, entries_);
            if (id == kInvalidReservation || id >= entries_.size() ||
                entries_[id].frame != kInvalidFrame) {
                break;
            }
            entries_[id] = ReservationEntry{frame, false};
            on_assign(id, frame);
        }
    }

    [[nodiscard]] static ReservationId FirstFree(
        const std::size_t, const std::vector<ReservationEntry>& entries) noexcept {
        for (std::size_t id = 0; id < entries.size(); ++id) {
            if (entries[id].frame == kInvalidFrame) {
                return static_cast<ReservationId>(id);
            }
        }
        return kInvalidReservation;
    }

private:
    std::vector<ReservationEntry> entries_;
};

}  // namespace pv
