#pragma once

#include "model.h"

namespace pv {

class WorkQueue {
public:
    // The queue lock is also the publication boundary for slot data prepared
    // by main before a worker acquires the corresponding DecodeWork.
    bool TryPush(DecodeWork& work) {
        std::lock_guard lock(mutex_);
        if (stopped_) return false;
        queue_.push_back(std::move(work));
        available_.notify_one();
        return true;
    }

    bool Pop(DecodeWork& output, const std::stop_token stop) {
        std::unique_lock lock(mutex_);
        std::stop_callback callback(stop, [this] { available_.notify_all(); });
        available_.wait(lock, [&] { return stopped_ || stop.stop_requested() || !queue_.empty(); });
        if (stopped_ || stop.stop_requested()) return false;
        output = std::move(queue_.front());
        queue_.pop_front();
        return true;
    }

    bool TryCancel(const SlotId staging_slot, DecodeWork& cancelled) {
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

    void Reorder(const std::span<const std::size_t> priority) {
        std::lock_guard lock(mutex_);
        const auto rank = [&](const std::size_t frame) {
            const auto found = std::find(priority.begin(), priority.end(), frame);
            return found == priority.end()
                       ? priority.size()
                       : static_cast<std::size_t>(found - priority.begin());
        };
        std::stable_sort(queue_.begin(), queue_.end(),
                         [&](const DecodeWork& left, const DecodeWork& right) {
                             return rank(left.index) < rank(right.index);
                         });
    }

    void Remap(const std::size_t from, const std::size_t to,
               const std::uint64_t generation) {
        std::lock_guard lock(mutex_);
        for (DecodeWork& work : queue_) {
            if (work.index == from && work.generation == generation) {
                work.index = to;
            }
        }
    }

    void Stop() {
        std::lock_guard lock(mutex_);
        stopped_ = true;
        available_.notify_all();
    }

    [[nodiscard]] std::size_t Size() const {
        std::lock_guard lock(mutex_);
        return queue_.size();
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable available_;
    std::deque<DecodeWork> queue_;
    bool stopped_ = false;
};

class CompletionQueue {
public:
    explicit CompletionQueue(const std::size_t capacity) : capacity_(capacity) {
        queue_.reserve(capacity);
        released_inputs_.reserve(capacity);
        event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!event_) ThrowLastError("CreateEventW(worker completion)");
    }

    ~CompletionQueue() { CloseHandle(event_); }

    CompletionQueue(const CompletionQueue&) = delete;
    CompletionQueue& operator=(const CompletionQueue&) = delete;

    // Draining under the same lock makes all worker writes to completed slot
    // resources visible before main inspects or recycles those slots.
    struct Batch {
        explicit Batch(const std::size_t capacity) {
            results.reserve(capacity);
            released_inputs.reserve(capacity);
        }

        std::vector<DecodeResult> results;
        std::vector<ReleasedInput> released_inputs;
    };

    void Push(DecodeResult result) noexcept {
        std::lock_guard lock(mutex_);
        if (queue_.size() == capacity_) std::terminate();
        queue_.push_back(std::move(result));
        if (!SetEvent(event_)) std::terminate();
    }

    void DrainAll(Batch& batch) noexcept {
        batch.results.clear();
        batch.released_inputs.clear();
        if (batch.results.capacity() < capacity_ ||
            batch.released_inputs.capacity() < capacity_) {
            std::terminate();
        }
        std::lock_guard lock(mutex_);
        if (!ResetEvent(event_)) std::terminate();
        batch.results.swap(queue_);
        batch.released_inputs.swap(released_inputs_);
    }

    void PushReleasedInput(ReleasedInput input) noexcept {
        std::lock_guard lock(mutex_);
        if (released_inputs_.size() == capacity_) {
            std::terminate();
        }
        released_inputs_.push_back(std::move(input));
        if (!SetEvent(event_)) std::terminate();
    }

    [[nodiscard]] HANDLE CompletionEvent() const noexcept { return event_; }

private:
    const std::size_t capacity_;
    std::mutex mutex_;
    std::vector<DecodeResult> queue_;
    std::vector<ReleasedInput> released_inputs_;
    HANDLE event_ = nullptr;
};

}  // namespace pv
