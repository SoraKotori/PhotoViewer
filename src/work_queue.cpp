#include "work_queue.h"

#include "win32_support.h"

#include <algorithm>

namespace pv {

WorkQueue::WorkQueue(const std::size_t capacity) : capacity_(capacity) {
    queue_.reserve(capacity);
    reorder_scratch_.reserve(capacity);
}

bool WorkQueue::TryPush(DecodeWork& work) {
    std::lock_guard lock(mutex_);
    if (stopped_ || queue_.size() == capacity_) return false;
    queue_.push_back(std::move(work));
    available_.notify_one();
    return true;
}

bool WorkQueue::Pop(DecodeWork& output, const std::stop_token stop) {
    std::unique_lock lock(mutex_);
    std::stop_callback callback(stop, [this] { available_.notify_all(); });
    available_.wait(lock, [&] {
        return stopped_ || stop.stop_requested() || !queue_.empty();
    });
    if (stopped_ || stop.stop_requested()) return false;
    output = std::move(queue_.front());
    queue_.erase(queue_.begin());
    return true;
}

bool WorkQueue::TryCancel(const SlotId staging_slot, DecodeWork& cancelled) {
    if (staging_slot == kInvalidSlot) return false;
    std::lock_guard lock(mutex_);
    const auto found = std::find_if(
        queue_.begin(), queue_.end(),
        [&](const DecodeWork& work) {
            return work.staging_slot == staging_slot;
        });
    if (found == queue_.end()) return false;
    cancelled = std::move(*found);
    queue_.erase(found);
    return true;
}

void WorkQueue::Reorder(const std::span<const std::size_t> priority) {
    std::lock_guard lock(mutex_);
    reorder_scratch_.clear();
    for (const std::size_t frame : priority) {
        const auto found = std::find_if(
            queue_.begin(), queue_.end(),
            [&](const DecodeWork& work) { return work.index == frame; });
        if (found == queue_.end()) continue;
        reorder_scratch_.push_back(std::move(*found));
        queue_.erase(found);
    }
    for (DecodeWork& work : queue_) {
        reorder_scratch_.push_back(std::move(work));
    }
    queue_.swap(reorder_scratch_);
}

void WorkQueue::Remap(const std::size_t from, const std::size_t to,
                      const std::uint64_t generation) {
    std::lock_guard lock(mutex_);
    for (DecodeWork& work : queue_) {
        if (work.index == from && work.generation == generation) {
            work.index = to;
        }
    }
}

void WorkQueue::Stop() {
    std::lock_guard lock(mutex_);
    stopped_ = true;
    available_.notify_all();
}

std::size_t WorkQueue::Size() const {
    std::lock_guard lock(mutex_);
    return queue_.size();
}

CompletionQueue::Batch::Batch(const std::size_t capacity) {
    results.reserve(capacity);
    released_inputs.reserve(capacity);
}

CompletionQueue::CompletionQueue(const std::size_t capacity)
    : capacity_(capacity) {
    queue_.reserve(capacity);
    released_inputs_.reserve(capacity);
    event_.Reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event_) ThrowLastError("CreateEventW(worker completion)");
}

void CompletionQueue::Push(DecodeResult result) noexcept {
    std::lock_guard lock(mutex_);
    if (queue_.size() == capacity_) std::terminate();
    queue_.push_back(std::move(result));
    if (!SetEvent(event_.Get())) std::terminate();
}

void CompletionQueue::PushReleasedInput(ReleasedInput input) noexcept {
    std::lock_guard lock(mutex_);
    if (released_inputs_.size() == capacity_) std::terminate();
    released_inputs_.push_back(std::move(input));
    if (!SetEvent(event_.Get())) std::terminate();
}

void CompletionQueue::DrainAll(Batch& batch) noexcept {
    batch.results.clear();
    batch.released_inputs.clear();
    if (batch.results.capacity() < capacity_ ||
        batch.released_inputs.capacity() < capacity_) std::terminate();
    std::lock_guard lock(mutex_);
    if (!ResetEvent(event_.Get())) std::terminate();
    batch.results.swap(queue_);
    batch.released_inputs.swap(released_inputs_);
}

HANDLE CompletionQueue::CompletionEvent() const noexcept {
    return event_.Get();
}

}  // namespace pv
