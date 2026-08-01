#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>

class NavigationController final
{
public:
    void initialize(std::size_t initialIndex, std::size_t imageCount);

    [[nodiscard]] bool onArrowKeyDown(int direction, bool repeated);
    [[nodiscard]] bool onArrowKeyUp(int direction);
    [[nodiscard]] bool onPresented(std::size_t index);

    [[nodiscard]] std::size_t displayedIndex() const noexcept { return displayedIndex_; }
    [[nodiscard]] std::optional<std::size_t> foregroundTarget() const noexcept { return foregroundTarget_; }
    [[nodiscard]] std::uint64_t targetSequence() const noexcept { return targetSequence_; }
    [[nodiscard]] int prefetchDirection() const noexcept;
    [[nodiscard]] bool hasPresentedImage() const noexcept { return hasPresentedImage_; }
    [[nodiscard]] bool isHolding() const noexcept { return repeatSeen_ && pressedDirection_ != 0; }

private:
    enum class TargetKind
    {
        Initial,
        Tap,
        Hold
    };

    [[nodiscard]] bool createAdjacentTarget(int direction, TargetKind kind);
    [[nodiscard]] bool activateNextTap();
    void cancelForeground();

    std::size_t imageCount_{};
    std::size_t displayedIndex_{};
    std::optional<std::size_t> foregroundTarget_;
    std::deque<int> pendingTaps_;
    TargetKind targetKind_{TargetKind::Initial};
    std::uint64_t targetSequence_{};
    std::size_t pendingHoldSteps_{};
    int pressedDirection_{};
    bool repeatSeen_{};
    bool hasPresentedImage_{};
};
