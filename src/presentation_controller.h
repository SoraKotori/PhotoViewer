#pragma once

#include "pipeline_types.h"

#include <cstdint>
#include <optional>

namespace pv {

// Serializes frame-credit, redraw and draw-fence ownership. A draw cannot
// start without credit and its texture slot is released exactly once.
class PresentationController {
public:
    [[nodiscard]] bool NeedsFrameCreditEvent() const noexcept;
    [[nodiscard]] bool CanDraw() const noexcept;
    [[nodiscard]] bool RedrawPending() const noexcept;
    [[nodiscard]] std::uint64_t DrawFence() const noexcept;
    [[nodiscard]] std::uint64_t ArmedFence() const noexcept;

    void GrantFrameCredit() noexcept;
    void RequestRedraw(bool grant_credit) noexcept;
    void StartDraw(SlotId slot, std::uint64_t fence);
    [[nodiscard]] std::optional<SlotId> CompleteDraw(
        std::uint64_t completed_fence) noexcept;
    void SetArmedFence(std::uint64_t fence) noexcept;

private:
    bool frame_credit_ = false;
    bool redraw_pending_ = true;
    std::uint64_t armed_fence_ = 0;
    SlotId reading_slot_ = kInvalidSlot;
    std::uint64_t reading_fence_ = 0;
};

}  // namespace pv
