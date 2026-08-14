#include "presentation_controller.h"

#include <stdexcept>

namespace pv {

bool PresentationController::NeedsFrameCreditEvent() const noexcept {
    return !frame_credit_;
}

bool PresentationController::CanDraw() const noexcept {
    return frame_credit_ && reading_fence_ == 0;
}

bool PresentationController::RedrawPending() const noexcept {
    return redraw_pending_;
}

std::uint64_t PresentationController::DrawFence() const noexcept {
    return reading_fence_;
}

std::uint64_t PresentationController::ArmedFence() const noexcept {
    return armed_fence_;
}

void PresentationController::GrantFrameCredit() noexcept {
    frame_credit_ = true;
}

void PresentationController::RequestRedraw(const bool grant_credit) noexcept {
    redraw_pending_ = true;
    if (grant_credit) frame_credit_ = true;
}

void PresentationController::StartDraw(const SlotId slot,
                                       const std::uint64_t fence) {
    if (!CanDraw() || slot == kInvalidSlot || fence == 0) {
        throw std::logic_error("invalid presentation draw handoff");
    }
    reading_slot_ = slot;
    reading_fence_ = fence;
    frame_credit_ = false;
    redraw_pending_ = false;
}

std::optional<SlotId> PresentationController::CompleteDraw(
    const std::uint64_t completed_fence) noexcept {
    if (reading_fence_ == 0 || reading_fence_ > completed_fence) {
        return std::nullopt;
    }
    const SlotId completed_slot = reading_slot_;
    reading_slot_ = kInvalidSlot;
    reading_fence_ = 0;
    return completed_slot == kInvalidSlot ? std::nullopt
                                          : std::optional<SlotId>{completed_slot};
}

void PresentationController::SetArmedFence(
    const std::uint64_t fence) noexcept {
    armed_fence_ = fence;
}

}  // namespace pv
