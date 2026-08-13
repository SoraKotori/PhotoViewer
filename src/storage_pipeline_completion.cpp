#include "storage_pipeline.h"

#include "pipeline_state.h"
#include "png.h"
#include "runtime_telemetry.h"

namespace pv {

bool StoragePipeline::DrainCompletions() {
    bool drained = false;
    std::array<IoCompletion, 32> completions{};
    for (;;) {
        const std::size_t removed = transport_.DrainAvailable(completions);
        if (removed == 0) break;
        for (std::size_t index = 0; index < removed; ++index) {
            const IoCompletion& completion = completions[index];
            OnCompletion(completion.request, completion.overlapped,
                         completion.result, completion.transferred);
        }
        drained = true;
    }
    return drained;
}

void StoragePipeline::OnCompletion(IoRequest* const request,
                                   OVERLAPPED* const overlapped,
                                   const DWORD result,
                                   const ULONG_PTR transferred) {
    if (!request || !overlapped || !request->file) return;
    if (overlapped == &request->header_overlapped) {
        OnHeaderReady(request, result, transferred);
    } else if (overlapped == &request->content_overlapped) {
        OnContentReady(request, result, transferred);
    }
}

void StoragePipeline::OnHeaderReady(IoRequest* request,
                                    const DWORD result,
                                    const ULONG_PTR transferred) {
    if (!request || request->index >= model_.FrameCount()) return;
    const ImageRecord& image = frames_.View(request->index);
    if (frames_.Io(request->index) != request ||
        request->generation != image.Generation() || request->header_completed) {
        return;
    }
    request->header_result = result;
    request->header_transferred = transferred;
    request->header_completed = true;
    if (request->index == model_.NavigationView().CurrentIndex() &&
        model_.NavigationView().InitialPending() &&
        telemetry_.At(StartupMilestone::InitialHeaderReady) ==
            std::chrono::steady_clock::time_point{}) {
        telemetry_.Mark(StartupMilestone::InitialHeaderReady);
    }
    bool accepted = false;
    if (request->header_result == ERROR_SUCCESS &&
        transferred >= kPngHeaderBytes) {
        const auto plan = ParsePngResourcePlan(std::span<const std::byte>(
            request->destination, transferred));
        accepted = plan &&
                   plan->staging_committed_bytes <=
                       limits_.staging_cache_bytes &&
                   plan->gpu_reservation_bytes <= limits_.gpu_cache_bytes;
        if (accepted) {
            catalog_.RecordResourcePlan(request->index, *plan);
            header_ready_frames_.push_back(request->index);
        }
    }
    if (!accepted) {
        frames_.MarkFailed(request->index);
        if (request->content_submitted && !request->content_completed) {
            (void)transport_.RequestCancellation(*request);
        }
    }
    if (!request->content_submitted || request->content_completed) {
        if (!request->content_submitted) {
            request->result = ERROR_SUCCESS;
            request->transferred = 0;
        }
        FinishRead(request);
    }
}

void StoragePipeline::OnContentReady(IoRequest* request, const DWORD result,
                                     const ULONG_PTR transferred) {
    if (request && request->index < model_.FrameCount()) {
        if (frames_.Io(request->index) == request &&
            !request->content_completed) {
            if (request->result == ERROR_IO_PENDING || result != ERROR_SUCCESS) {
                request->result = result;
            }
            request->transferred += transferred;
            request->content_completed = true;
            if (request->index == model_.NavigationView().CurrentIndex() &&
                model_.NavigationView().InitialPending() &&
                telemetry_.At(
                    StartupMilestone::InitialContentCompletionObserved) ==
                    std::chrono::steady_clock::time_point{}) {
                // IOCP packets do not include a kernel completion timestamp.
                // Synchronous success is observed here immediately; pending
                // I/O is observed when main dequeues its completion packet.
                telemetry_.Mark(
                    StartupMilestone::InitialContentCompletionObserved);
            }
            if (!request->split_header || request->header_completed) {
                FinishRead(request);
            }
        }
    }
}

void StoragePipeline::FinishRead(IoRequest* const request) {
    if (!request || request->index >= model_.FrameCount()) return;
    const ImageRecord& image = frames_.View(request->index);
    if (frames_.Io(request->index) != request) return;

    request->file.Reset();

    const DWORD io_result = request->result;
    const std::size_t transferred = request->transferred;
    const SlotId compressed_slot = request->compressed_slot;
    const CompressedSlot& slot = slots_.Compressed(compressed_slot);
    const std::size_t allocation = slot.Buffer().size;
    const bool header_success = !request->split_header ||
        (request->header_result == ERROR_SUCCESS &&
         request->header_transferred == request->prefix_bytes);
    const std::size_t expected_content = request->split_header
                                             ? allocation - request->prefix_bytes
                                             : allocation;
    const bool success = header_success && io_result == ERROR_SUCCESS &&
                         transferred == expected_content;
    const bool current = request->generation == image.Generation();
    frames_.DetachIo(request->index, request);
    if (request->index == model_.NavigationView().CurrentIndex() &&
        model_.NavigationView().InitialPending() &&
        telemetry_.At(StartupMilestone::InitialContentReady) ==
            std::chrono::steady_clock::time_point{}) {
        telemetry_.Mark(StartupMilestone::InitialContentReady);
        initial_content_completed_ = true;
    }

    const bool reserved = current && IsReservationActive(
        model_.ReservationPlan().Compressed(), image.CompressedReservation(),
        request->index);
    if (success && reserved && !image.Failed() &&
        model_.CatalogItemAt(request->index).header_valid) {
        slots_.CompleteFileRead(compressed_slot);
    } else {
        slots_.ReleaseCompressed(compressed_slot);
        frames_.ClearCompressedSlot(request->index, compressed_slot);
        if (reserved && io_result != ERROR_OPERATION_ABORTED) {
            frames_.MarkFailed(request->index);
        }
    }
}

}  // namespace pv
