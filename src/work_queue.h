#pragma once

#include "model.h"

namespace pv {

class WorkQueue {
public:
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

    bool TryCancel(WorkToken* const token, DecodeWork& cancelled) {
        if (!token) return false;
        std::lock_guard lock(mutex_);
        const auto found = std::find_if(
            queue_.begin(), queue_.end(),
            [&](const DecodeWork& work) { return work.token == token; });
        if (found == queue_.end()) return false;
        WorkClaim expected = WorkClaim::Queued;
        if (!token->claim.compare_exchange_strong(
                expected, WorkClaim::Cancelled, std::memory_order_acq_rel)) {
            return false;
        }
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
    struct Batch {
        std::vector<DecodeResult> results;
        std::vector<ReleasedInput> released_inputs;
    };

    [[nodiscard]] bool Push(DecodeResult result) {
        std::lock_guard lock(mutex_);
        queue_.push_back(std::move(result));
        if (notification_pending_) return false;
        notification_pending_ = true;
        return true;
    }

    Batch DrainAll() {
        std::lock_guard lock(mutex_);
        Batch batch;
        batch.results.reserve(queue_.size());
        while (!queue_.empty()) {
            batch.results.push_back(std::move(queue_.front()));
            queue_.pop_front();
        }
        batch.released_inputs.reserve(released_inputs_.size());
        while (!released_inputs_.empty()) {
            batch.released_inputs.push_back(std::move(released_inputs_.front()));
            released_inputs_.pop_front();
        }
        notification_pending_ = false;
        return batch;
    }

    [[nodiscard]] bool PushReleasedInput(ReleasedInput input) {
        std::lock_guard lock(mutex_);
        released_inputs_.push_back(std::move(input));
        if (notification_pending_) return false;
        notification_pending_ = true;
        return true;
    }

private:
    std::mutex mutex_;
    std::deque<DecodeResult> queue_;
    std::deque<ReleasedInput> released_inputs_;
    bool notification_pending_ = false;
};

}  // namespace pv
