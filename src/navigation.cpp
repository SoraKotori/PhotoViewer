#include "navigation.h"

#include <algorithm>
#include <cassert>

namespace pv {

void NavigationState::Reset(const std::size_t initial_index, const std::size_t image_count) {
    image_count_ = image_count;
    current_index_ = initial_index;
    initial_pending_ = image_count != 0;
    held_direction_ = 0;
    committed_.clear();
    repeated_.clear();
}

std::size_t NavigationState::Apply(const std::size_t value, const int direction) noexcept {
    return direction < 0 ? value - 1 : value + 1;
}

std::size_t NavigationState::ProjectedIndex() const noexcept {
    std::size_t value = current_index_;
    for (const int direction : committed_) value = Apply(value, direction);
    for (const int direction : repeated_) value = Apply(value, direction);
    return value;
}

void NavigationState::Step(const int direction, const bool repeat) {
    if (image_count_ == 0 || (direction != -1 && direction != 1)) return;
    if (held_direction_ != 0 && held_direction_ != direction) repeated_.clear();
    held_direction_ = direction;
    const std::size_t projected = ProjectedIndex();
    if ((direction < 0 && projected == 0) ||
        (direction > 0 && projected + 1 >= image_count_)) {
        return;
    }
    (repeat ? repeated_ : committed_).push_back(direction);
}

void NavigationState::Release(const int direction) {
    if (held_direction_ == direction) {
        held_direction_ = 0;
        repeated_.clear();
    }
}

std::optional<std::size_t> NavigationState::NextIndex() const {
    if (image_count_ == 0) return std::nullopt;
    if (initial_pending_) return current_index_;
    if (!committed_.empty()) return Apply(current_index_, committed_.front());
    if (!repeated_.empty()) return Apply(current_index_, repeated_.front());
    return std::nullopt;
}

void NavigationState::CompletePresentation(const std::size_t index) {
    if (initial_pending_) {
        assert(index == current_index_);
        initial_pending_ = false;
        return;
    }
    if (!committed_.empty()) {
        assert(index == Apply(current_index_, committed_.front()));
        committed_.pop_front();
    } else if (!repeated_.empty()) {
        assert(index == Apply(current_index_, repeated_.front()));
        repeated_.pop_front();
    } else {
        assert(index == current_index_);
    }
    current_index_ = index;
}

std::pair<std::size_t, std::size_t> NavigationState::RequiredBounds() const {
    if (image_count_ == 0) return {0, 0};
    std::size_t low = current_index_;
    std::size_t high = current_index_;
    std::size_t value = current_index_;
    auto include = [&](const int direction) {
        value = Apply(value, direction);
        low = std::min(low, value);
        high = std::max(high, value);
    };
    for (const int direction : committed_) include(direction);
    for (const int direction : repeated_) include(direction);
    return {low, high};
}

int NavigationState::PreferredDirection() const noexcept {
    if (held_direction_ != 0) return held_direction_;
    if (!committed_.empty()) return committed_.front();
    if (!repeated_.empty()) return repeated_.front();
    return 1;
}

}  // namespace pv
