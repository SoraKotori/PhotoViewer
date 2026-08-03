#pragma once

#include "model.h"

namespace pv {

class WorkQueue {
public:
    explicit WorkQueue(const std::size_t capacity) : capacity_(capacity) {}

    bool TryPush(DecodeWork& work) {
        std::lock_guard lock(mutex_);
        if (stopped_ || queue_.size() >= capacity_) return false;
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
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable available_;
    std::deque<DecodeWork> queue_;
    bool stopped_ = false;
};

class CompletionQueue {
public:
    void Push(DecodeResult result) {
        std::lock_guard lock(mutex_);
        queue_.push_back(std::move(result));
    }

    std::vector<DecodeResult> Drain() {
        std::lock_guard lock(mutex_);
        std::vector<DecodeResult> results;
        results.reserve(queue_.size());
        while (!queue_.empty()) {
            results.push_back(std::move(queue_.front()));
            queue_.pop_front();
        }
        return results;
    }

    void PushReleasedInput(ReleasedInput input) {
        std::lock_guard lock(mutex_);
        released_inputs_.push_back(std::move(input));
    }

    std::vector<ReleasedInput> DrainReleasedInputs() {
        std::lock_guard lock(mutex_);
        std::vector<ReleasedInput> inputs;
        inputs.reserve(released_inputs_.size());
        while (!released_inputs_.empty()) {
            inputs.push_back(std::move(released_inputs_.front()));
            released_inputs_.pop_front();
        }
        return inputs;
    }

private:
    std::mutex mutex_;
    std::deque<DecodeResult> queue_;
    std::deque<ReleasedInput> released_inputs_;
};

}  // namespace pv
