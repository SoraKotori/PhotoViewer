#include "navigation_controller.h"

void NavigationController::initialize(const std::size_t initialIndex, const std::size_t imageCount)
{
    imageCount_ = imageCount;
    displayedIndex_ = initialIndex;
    foregroundTarget_ = initialIndex;
    pendingTaps_.clear();
    targetKind_ = TargetKind::Initial;
    targetSequence_ = 1;
    pendingHoldSteps_ = 0;
    pressedDirection_ = 0;
    repeatSeen_ = false;
    hasPresentedImage_ = false;
}

bool NavigationController::onArrowKeyDown(const int direction, const bool repeated)
{
    const int normalizedDirection = direction < 0 ? -1 : 1;
    if (repeated) {
        if (pressedDirection_ != normalizedDirection) {
            return false;
        }
        repeatSeen_ = true;
        if (pendingHoldSteps_ < imageCount_) {
            ++pendingHoldSteps_;
        }
        if (!foregroundTarget_ && hasPresentedImage_) {
            --pendingHoldSteps_;
            return createAdjacentTarget(normalizedDirection, TargetKind::Hold);
        }
        return true;
    }

    const bool changedDirection = pressedDirection_ != 0 && pressedDirection_ != normalizedDirection;
    pressedDirection_ = normalizedDirection;
    repeatSeen_ = false;
    pendingHoldSteps_ = 0;

    if (!hasPresentedImage_) {
        return false;
    }
    if (changedDirection && foregroundTarget_) {
        cancelForeground();
        pendingTaps_.clear();
    }
    if (!foregroundTarget_) {
        return createAdjacentTarget(normalizedDirection, TargetKind::Tap);
    }
    pendingTaps_.push_back(normalizedDirection);
    return true;
}

bool NavigationController::onArrowKeyUp(const int direction)
{
    const int normalizedDirection = direction < 0 ? -1 : 1;
    if (pressedDirection_ != normalizedDirection) {
        return false;
    }

    pressedDirection_ = 0;
    const bool wasHolding = repeatSeen_;
    repeatSeen_ = false;
    pendingHoldSteps_ = 0;
    if (wasHolding) {
        if (foregroundTarget_ && targetKind_ != TargetKind::Initial) {
            cancelForeground();
        } else {
            ++targetSequence_;
        }
        pendingTaps_.clear();
        return true;
    }
    return false;
}

bool NavigationController::onPresented(const std::size_t index)
{
    if (!foregroundTarget_ || *foregroundTarget_ != index) {
        return false;
    }

    displayedIndex_ = index;
    hasPresentedImage_ = true;
    foregroundTarget_.reset();
    if (repeatSeen_ && pressedDirection_ != 0 && pendingHoldSteps_ != 0) {
        --pendingHoldSteps_;
        static_cast<void>(createAdjacentTarget(pressedDirection_, TargetKind::Hold));
    } else {
        static_cast<void>(activateNextTap());
    }
    return true;
}

int NavigationController::prefetchDirection() const noexcept
{
    if (pressedDirection_ != 0) {
        return pressedDirection_;
    }
    if (foregroundTarget_) {
        if (*foregroundTarget_ > displayedIndex_) {
            return 1;
        }
        if (*foregroundTarget_ < displayedIndex_) {
            return -1;
        }
    }
    return 0;
}

bool NavigationController::createAdjacentTarget(const int direction, const TargetKind kind)
{
    if (imageCount_ == 0) {
        return false;
    }

    std::size_t next = displayedIndex_;
    if (direction > 0 && displayedIndex_ + 1 < imageCount_) {
        next = displayedIndex_ + 1;
    } else if (direction < 0 && displayedIndex_ > 0) {
        next = displayedIndex_ - 1;
    }
    if (next == displayedIndex_) {
        return false;
    }

    foregroundTarget_ = next;
    targetKind_ = kind;
    ++targetSequence_;
    return true;
}

bool NavigationController::activateNextTap()
{
    while (!pendingTaps_.empty()) {
        const int direction = pendingTaps_.front();
        pendingTaps_.pop_front();
        if (createAdjacentTarget(direction, TargetKind::Tap)) {
            return true;
        }
    }
    return false;
}

void NavigationController::cancelForeground()
{
    foregroundTarget_.reset();
    ++targetSequence_;
}
