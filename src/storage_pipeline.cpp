#include "storage_pipeline.h"

#include "pipeline_model.h"
#include "pipeline_resource_size.h"
#include "pipeline_resources.h"
#include "pipeline_state.h"
#include "runtime_telemetry.h"
#include "win32_support.h"

#include <numeric>

namespace pv {

StoragePipeline::StoragePipeline(const PipelineLimits& limits,
                                 const PipelineModel& model,
                                 StorageCatalogAccess catalog,
                                 StorageFrameAccess frames,
                                 const PipelineResources& resources,
                                 StorageResourceAccess slots,
                                 RuntimeTelemetry& telemetry)
    : limits_(limits), model_(model), catalog_(catalog), frames_(frames),
      resources_(resources),
      slots_(slots), telemetry_(telemetry) {
    header_ready_frames_.reserve(limits.compressed_slot_count);
}

StoragePipeline::~StoragePipeline() {
    if (!shutdown_) Shutdown();
}

HANDLE StoragePipeline::CompletionEvent() const noexcept {
    return transport_.CompletionEvent();
}

bool StoragePipeline::Enabled() const noexcept { return transport_.Enabled(); }

std::size_t StoragePipeline::TransferGranularity() const noexcept {
    return transport_.CachedTransferGranularity();
}

std::size_t StoragePipeline::CompressedAlignment() const noexcept {
    const std::size_t transfer = transport_.CachedTransferGranularity();
    return transfer == 0 ? 0 : std::lcm<std::size_t>(4096, transfer);
}

bool StoragePipeline::InitialContentCompleted() const noexcept {
    return initial_content_completed_;
}

std::span<const std::size_t> StoragePipeline::HeaderReadyFrames() const noexcept {
    return header_ready_frames_;
}

void StoragePipeline::ClearHeaderReadyFrames() noexcept {
    header_ready_frames_.clear();
}

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
    if (request->header_result == ERROR_SUCCESS && transferred >= 24) {
        const auto header = ParsePngHeader(std::span<const std::byte>(
            request->destination, 24));
        if (header) {
            catalog_.RecordHeader(request->index, *header);
            header_ready_frames_.push_back(request->index);
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
                telemetry_.At(StartupMilestone::InitialContentCompletionObserved) ==
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
    if (success && reserved) {
        const auto header = ParsePngHeader(std::span<const std::byte>(
            slot.Buffer().data, slot.Buffer().size));
        if (header) {
            catalog_.RecordHeader(request->index, *header);
            slots_.CompleteFileRead(compressed_slot);
        } else {
            slots_.ReleaseCompressed(compressed_slot);
            frames_.ClearCompressedSlot(request->index, compressed_slot);
            frames_.MarkFailed(request->index);
        }
    } else {
        slots_.ReleaseCompressed(compressed_slot);
        frames_.ClearCompressedSlot(request->index, compressed_slot);
        if (reserved && io_result != ERROR_OPERATION_ABORTED) {
            frames_.MarkFailed(request->index);
        }
    }
}

void StoragePipeline::SubmitEligibleReads() {
    struct ReadSubmission {
        DWORD result = ERROR_SUCCESS;
        DWORD transferred = 0;
        bool completed = false;
    };
    const auto submit_read = [&](IoRequest& request, OVERLAPPED& overlapped,
                                 const DWORD buffer_offset, const DWORD bytes,
                                 const std::uint64_t file_offset) {
        overlapped = {};
        overlapped.hEvent = transport_.CompletionEvent();
        overlapped.Offset = static_cast<DWORD>(file_offset);
        overlapped.OffsetHigh = static_cast<DWORD>(file_offset >> 32U);
        if (ReadFile(request.file.Get(), request.destination + buffer_offset, bytes,
                     nullptr, &overlapped)) {
            DWORD transferred = 0;
            if (!GetOverlappedResult(request.file.Get(), &overlapped,
                                     &transferred, FALSE)) {
                return ReadSubmission{GetLastError(), 0, true};
            }
            return ReadSubmission{ERROR_SUCCESS, transferred, true};
        }
        const DWORD error = GetLastError();
        return error == ERROR_IO_PENDING
                   ? ReadSubmission{}
                   : ReadSubmission{error, 0, true};
    };
    const auto submit_request = [&](const std::size_t index) {
        IoRequest* const request = frames_.Io(index);
        ReadSubmission header;
        ReadSubmission content;
        if (request->split_header) {
            request->prefix_bytes = std::min(
                request->byte_count,
                transport_.CachedTransferGranularity());
            const DWORD prefix_transfer = std::min(
                request->transfer_count,
                transport_.CachedTransferGranularity());
            request->header_submitted = true;
            header = telemetry_.Measure(
                TimedOperation::SubmitFileReads,
                [&] {
                    return submit_read(*request, request->header_overlapped,
                                       0, prefix_transfer, 0);
                });
            if (prefix_transfer < request->transfer_count) {
                request->result = ERROR_SUCCESS;
                request->content_submitted = true;
                content = telemetry_.Measure(
                    TimedOperation::SubmitFileReads,
                    [&] {
                        return submit_read(
                            *request, request->content_overlapped,
                            prefix_transfer,
                            request->transfer_count - prefix_transfer,
                            prefix_transfer);
                    });
            }
        } else {
            request->result = ERROR_SUCCESS;
            request->content_submitted = true;
            content = telemetry_.Measure(
                TimedOperation::SubmitFileReads,
                [&] {
                    return submit_read(*request, request->content_overlapped,
                                       0, request->transfer_count, 0);
                });
        }
        if (header.completed && frames_.Io(index) == request) {
            OnHeaderReady(request, header.result, header.transferred);
        }
        if (content.completed && frames_.Io(index) == request) {
            OnContentReady(request, content.result, content.transferred);
        }
    };
    const bool initial_content_pending =
        model_.NavigationView().InitialPending() &&
        !initial_content_completed_;
    const std::size_t initial_index = model_.NavigationView().CurrentIndex();
    bool prepared_reads = false;
    for (const std::size_t index : model_.ReservationPlan().PriorityOrder()) {
        if (initial_content_pending && index != initial_index) continue;
        if (DeterminePipelineStage(index, frames_.View(index), resources_,
                                   model_.ReservationPlan()) !=
            PipelineStage::WaitingIo) {
            continue;
        }
        const ImageRecord& image = frames_.View(index);
        const CatalogItem& item = model_.CatalogItemAt(index);
        HANDLE opened_file = telemetry_.Measure(
            TimedOperation::OpenFile,
            [&] {
                return CreateFileW(
                    item.path.c_str(), GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED |
                        FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_NO_BUFFERING,
                    nullptr);
            });
        if (opened_file == INVALID_HANDLE_VALUE) {
            frames_.MarkFailed(index);
            continue;
        }
        if (!item.file_size_known) {
            LARGE_INTEGER file_size{};
            if (!GetFileSizeEx(opened_file, &file_size) ||
                file_size.QuadPart <= 0 ||
                static_cast<unsigned long long>(file_size.QuadPart) >
                    std::numeric_limits<DWORD>::max()) {
                CloseHandle(opened_file);
                frames_.MarkFailed(index);
                continue;
            }
            catalog_.RecordFileSize(
                index, static_cast<std::uint64_t>(file_size.QuadPart));
        }
        if (item.file_bytes <= 24 ||
            item.file_bytes > std::numeric_limits<DWORD>::max()) {
            CloseHandle(opened_file);
            frames_.MarkFailed(index);
            continue;
        }
        const std::size_t cached_granularity =
            transport_.CachedTransferGranularity();
        const std::size_t granularity =
            transport_.TransferGranularity(opened_file);
        if (granularity == 0) {
            CloseHandle(opened_file);
            frames_.MarkFailed(index);
            continue;
        }
        if (cached_granularity == 0 && granularity != 0) {
            catalog_.MarkReservationPlanDirty();
        }
        const auto transfer_bytes = CompressedReservationBytes(
            item, limits_.compressed_budget_bytes, CompressedAlignment());
        if (!transfer_bytes || *transfer_bytes > limits_.compressed_budget_bytes ||
            *transfer_bytes > std::numeric_limits<DWORD>::max()) {
            CloseHandle(opened_file);
            frames_.MarkFailed(index);
            continue;
        }
        const SlotId compressed_slot = telemetry_.Measure(
            TimedOperation::AcquireCompressed,
            [&] {
                return slots_.AcquireCompressed(
                    *transfer_bytes, index, image.Generation());
            });
        if (compressed_slot == kInvalidSlot) {
            CloseHandle(opened_file);
            if (initial_content_pending && index == initial_index) {
                frames_.MarkFailed(index);
            }
            continue;
        }
        frames_.AttachCompressedSlot(index, compressed_slot);
        IoRequest& request = slots_.FileReadRequest(compressed_slot);
        request.Reset();
        request.index = index;
        request.generation = image.Generation();
        request.compressed_slot = compressed_slot;
        request.file.Reset(opened_file);

        frames_.AttachIo(index, &request);
        CompressedBuffer& buffer =
            slots_.FileReadBuffer(compressed_slot);
        const std::size_t file_bytes = static_cast<std::size_t>(item.file_bytes);
        buffer.size = file_bytes;
        request.destination = buffer.data;
        request.byte_count = static_cast<DWORD>(file_bytes);
        request.transfer_count = static_cast<DWORD>(*transfer_bytes);
        request.split_header = !item.header_valid;
        telemetry_.Measure(
            TimedOperation::SubmitFileReads,
            [&] {
                transport_.Associate(opened_file, request);
            });
        transport_.ConfigureCompletionMode(opened_file);
        prepared_reads = true;
    }
    if (!prepared_reads) return;

    for (const std::size_t index : model_.ReservationPlan().PriorityOrder()) {
        IoRequest* const request = frames_.Io(index);
        if (!request || request->header_submitted ||
            request->content_submitted) {
            continue;
        }
        submit_request(index);
    }
}

void StoragePipeline::Shutdown() {
    std::size_t pending_operations = 0;
    for (std::size_t index = 0; index < model_.FrameCount(); ++index) {
        IoRequest* const request = frames_.Io(index);
        if (request && request->file) {
            (void)transport_.RequestCancellation(*request);
            if (request->header_submitted && !request->header_completed) {
                ++pending_operations;
            }
            if (request->content_submitted && !request->content_completed) {
                ++pending_operations;
            }
        }
    }
    while (pending_operations != 0) {
        const IoCompletion completion =
            transport_.WaitForShutdownCompletion();
        IoRequest* const request = completion.request;
        if (completion.overlapped == &request->header_overlapped &&
            request->header_submitted && !request->header_completed) {
            request->header_completed = true;
            --pending_operations;
        } else if (completion.overlapped == &request->content_overlapped &&
                   request->content_submitted &&
                   !request->content_completed) {
            request->content_completed = true;
            --pending_operations;
        }
    }
    for (std::size_t index = 0; index < model_.FrameCount(); ++index) {
        IoRequest* const request = frames_.Io(index);
        if (!request) continue;
        request->file.Reset();
        if (request->compressed_slot != kInvalidSlot) {
            slots_.ReleaseCompressed(request->compressed_slot);
            frames_.ClearCompressedSlot(index, request->compressed_slot);
        }
        frames_.DetachIo(index, request);
    }
    shutdown_ = true;
}

void StoragePipeline::RetireRead(const std::size_t frame) {
    const ImageRecord& image = frames_.View(frame);
    IoRequest* const request = frames_.Io(frame);
    if (!request) return;
    slots_.CancelFileRead(image.CompressedSlot());
    (void)transport_.RequestCancellation(*request);
}

void StoragePipeline::RemapActiveRead(const std::size_t destination) {
    IoRequest* const request = frames_.Io(destination);
    if (request) request->index = destination;
}

}  // namespace pv
