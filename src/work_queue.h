#pragma once

#include "pipeline_types.h"
#include "win32_handle.h"

#include <condition_variable>
#include <mutex>
#include <span>
#include <stop_token>
#include <vector>

namespace pv {

class WorkQueue {
public:
    explicit WorkQueue(std::size_t capacity);
    bool TryPush(DecodeWork& work);
    bool Pop(DecodeWork& output, std::stop_token stop);
    bool TryCancel(SlotId staging_slot, DecodeWork& cancelled);
    void Reorder(std::span<const std::size_t> priority);
    void Remap(std::size_t from, std::size_t to, std::uint64_t generation);
    void Stop();
    [[nodiscard]] std::size_t Size() const;

private:
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable available_;
    std::vector<DecodeWork> queue_;
    std::vector<DecodeWork> reorder_scratch_;
    bool stopped_ = false;
};

class CompletionQueue {
public:
    struct Batch {
        explicit Batch(std::size_t capacity);
        std::vector<DecodeResult> results;
        std::vector<ReleasedInput> released_inputs;
    };

    explicit CompletionQueue(std::size_t capacity);
    ~CompletionQueue() = default;
    CompletionQueue(const CompletionQueue&) = delete;
    CompletionQueue& operator=(const CompletionQueue&) = delete;

    void Push(DecodeResult result) noexcept;
    void PushReleasedInput(ReleasedInput input) noexcept;
    void DrainAll(Batch& batch) noexcept;
    [[nodiscard]] HANDLE CompletionEvent() const noexcept;

private:
    const std::size_t capacity_;
    std::mutex mutex_;
    std::vector<DecodeResult> queue_;
    std::vector<ReleasedInput> released_inputs_;
    UniqueHandle event_;
};

}  // namespace pv
