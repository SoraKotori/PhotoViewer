#pragma once

#include "work_queue.h"

namespace pv {

class DecoderPool {
public:
    DecoderPool(std::size_t worker_count, WorkQueue& work_queue,
                CompletionQueue& completion_queue, HWND event_window);
    ~DecoderPool();

    DecoderPool(const DecoderPool&) = delete;
    DecoderPool& operator=(const DecoderPool&) = delete;

private:
    void WorkerMain(std::stop_token stop);
    DecodeResult Decode(DecodeWork work);
    void ReleaseInput(DecodeWork& work) noexcept;

    WorkQueue& work_queue_;
    CompletionQueue& completion_queue_;
    HWND event_window_ = nullptr;
    std::vector<std::jthread> workers_;
};

}  // namespace pv
