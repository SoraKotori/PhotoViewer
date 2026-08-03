#pragma once

#include <cstddef>
#include <deque>
#include <optional>
#include <utility>

namespace pv {

class NavigationState {
public:
    void Reset(std::size_t initial_index, std::size_t image_count);
    void Step(int direction, bool repeat);
    void Release(int direction);

    [[nodiscard]] std::optional<std::size_t> NextIndex() const;
    void CompletePresentation(std::size_t index);

    [[nodiscard]] std::pair<std::size_t, std::size_t> RequiredBounds() const;
    [[nodiscard]] int PreferredDirection() const noexcept;
    [[nodiscard]] std::size_t CurrentIndex() const noexcept { return current_index_; }
    [[nodiscard]] bool InitialPending() const noexcept { return initial_pending_; }
    [[nodiscard]] bool Empty() const noexcept {
        return !initial_pending_ && committed_.empty() && repeated_.empty();
    }

private:
    [[nodiscard]] std::size_t ProjectedIndex() const noexcept;
    [[nodiscard]] static std::size_t Apply(std::size_t value, int direction) noexcept;

    std::size_t image_count_ = 0;
    std::size_t current_index_ = 0;
    bool initial_pending_ = false;
    int held_direction_ = 0;
    std::deque<int> committed_;
    std::deque<int> repeated_;
};

}  // namespace pv
