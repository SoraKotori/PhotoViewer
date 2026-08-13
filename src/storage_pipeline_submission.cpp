#include "storage_pipeline.h"

#include "pipeline_resource_size.h"
#include "pipeline_state.h"
#include "png.h"
#include "runtime_telemetry.h"

#include <algorithm>
#include <limits>

namespace pv {

HANDLE StoragePipeline::OpenReadFile(const std::size_t frame) {
    const CatalogItem& item = model_.CatalogItemAt(frame);
    return telemetry_.Measure(
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
}

std::optional<std::size_t> StoragePipeline::PlanReadTransfer(
    const std::size_t frame, const HANDLE file) {
    const CatalogItem& item = model_.CatalogItemAt(frame);
    if (!item.file_size_known) {
        LARGE_INTEGER file_size{};
        if (!GetFileSizeEx(file, &file_size) || file_size.QuadPart <= 0 ||
            static_cast<unsigned long long>(file_size.QuadPart) >
                std::numeric_limits<DWORD>::max()) {
            return std::nullopt;
        }
        catalog_.RecordFileSize(
            frame, static_cast<std::uint64_t>(file_size.QuadPart));
    }
    if (item.file_bytes < kPngHeaderBytes ||
        item.file_bytes > std::numeric_limits<DWORD>::max()) {
        return std::nullopt;
    }

    const std::size_t cached_granularity =
        transport_.CachedTransferGranularity();
    const std::size_t granularity = transport_.TransferGranularity(file);
    if (granularity == 0) return std::nullopt;
    if (cached_granularity == 0) {
        catalog_.MarkReservationPlanDirty();
    }

    const auto transfer_bytes = CompressedReservationBytes(
        item, limits_.compressed_budget_bytes, CompressedAlignment());
    if (!transfer_bytes ||
        *transfer_bytes > limits_.compressed_budget_bytes ||
        *transfer_bytes > std::numeric_limits<DWORD>::max()) {
        return std::nullopt;
    }
    return transfer_bytes;
}

bool StoragePipeline::PrepareRead(const std::size_t frame,
                                  const bool initial_content) {
    const ImageRecord& image = frames_.View(frame);
    HANDLE file = OpenReadFile(frame);
    if (file == INVALID_HANDLE_VALUE) {
        frames_.MarkFailed(frame);
        return false;
    }

    const auto transfer_bytes = PlanReadTransfer(frame, file);
    if (!transfer_bytes) {
        CloseHandle(file);
        frames_.MarkFailed(frame);
        return false;
    }

    const SlotId compressed_slot = telemetry_.Measure(
        TimedOperation::AcquireCompressed,
        [&] {
            return slots_.AcquireCompressed(
                *transfer_bytes, frame, image.Generation());
        });
    if (compressed_slot == kInvalidSlot) {
        CloseHandle(file);
        if (initial_content) frames_.MarkFailed(frame);
        return false;
    }

    frames_.AttachCompressedSlot(frame, compressed_slot);
    IoRequest& request = slots_.FileReadRequest(compressed_slot);
    request.Reset();
    request.index = frame;
    request.generation = image.Generation();
    request.compressed_slot = compressed_slot;
    request.file.Reset(file);
    frames_.AttachIo(frame, &request);

    const CatalogItem& item = model_.CatalogItemAt(frame);
    CompressedBuffer& buffer = slots_.FileReadBuffer(compressed_slot);
    const std::size_t file_bytes = static_cast<std::size_t>(item.file_bytes);
    buffer.size = file_bytes;
    request.destination = buffer.data;
    request.byte_count = static_cast<DWORD>(file_bytes);
    request.transfer_count = static_cast<DWORD>(*transfer_bytes);
    request.split_header = !item.header_valid;
    telemetry_.Measure(
        TimedOperation::SubmitFileReads,
        [&] { transport_.Associate(file, request); });
    transport_.ConfigureCompletionMode(file);
    return true;
}

bool StoragePipeline::PrepareEligibleReads() {
    const bool initial_content_pending =
        model_.NavigationView().InitialPending() &&
        !initial_content_completed_;
    const std::size_t initial_frame =
        model_.NavigationView().CurrentIndex();
    bool prepared = false;
    for (const std::size_t frame :
         model_.ReservationPlan().PriorityOrder()) {
        if (initial_content_pending && frame != initial_frame) continue;
        if (DeterminePipelineStage(frame, frames_.View(frame),
                                   resources_.SlotsView(),
                                   model_.ReservationPlan()) !=
            PipelineStage::WaitingIo) {
            continue;
        }
        const bool is_initial =
            initial_content_pending && frame == initial_frame;
        prepared = PrepareRead(frame, is_initial) || prepared;
    }
    return prepared;
}

StoragePipeline::ReadSubmission StoragePipeline::SubmitRead(
    IoRequest& request, OVERLAPPED& overlapped,
    const DWORD buffer_offset, const DWORD bytes,
    const std::uint64_t file_offset) {
    overlapped = {};
    overlapped.hEvent = transport_.CompletionEvent();
    overlapped.Offset = static_cast<DWORD>(file_offset);
    overlapped.OffsetHigh = static_cast<DWORD>(file_offset >> 32U);
    if (ReadFile(request.file.Get(), request.destination + buffer_offset,
                 bytes, nullptr, &overlapped)) {
        DWORD transferred = 0;
        if (!GetOverlappedResult(request.file.Get(), &overlapped,
                                 &transferred, FALSE)) {
            return {GetLastError(), 0, true};
        }
        return {ERROR_SUCCESS, transferred, true};
    }
    const DWORD error = GetLastError();
    return error == ERROR_IO_PENDING
               ? ReadSubmission{}
               : ReadSubmission{error, 0, true};
}

bool StoragePipeline::SubmitPreparedRead(const std::size_t frame) {
    IoRequest* const request = frames_.Io(frame);
    if (!request) return false;

    ReadSubmission header;
    ReadSubmission content;
    if (request->split_header) {
        request->prefix_bytes = std::min(
            request->byte_count, transport_.CachedTransferGranularity());
        const DWORD prefix_transfer = std::min(
            request->transfer_count,
            transport_.CachedTransferGranularity());
        request->header_submitted = true;
        header = telemetry_.Measure(
            TimedOperation::SubmitFileReads,
            [&] {
                return SubmitRead(*request, request->header_overlapped,
                                  0, prefix_transfer, 0);
            });
        if (prefix_transfer < request->transfer_count) {
            request->result = ERROR_SUCCESS;
            request->content_submitted = true;
            content = telemetry_.Measure(
                TimedOperation::SubmitFileReads,
                [&] {
                    return SubmitRead(
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
                return SubmitRead(*request, request->content_overlapped,
                                  0, request->transfer_count, 0);
            });
    }

    bool synchronous_progress = false;
    if (header.completed && frames_.Io(frame) == request) {
        synchronous_progress = true;
        OnHeaderReady(request, header.result, header.transferred);
    }
    if (content.completed && frames_.Io(frame) == request) {
        synchronous_progress = true;
        OnContentReady(request, content.result, content.transferred);
    }
    return synchronous_progress;
}

bool StoragePipeline::SubmitPreparedReads() {
    bool synchronous_progress = false;
    for (const std::size_t frame :
         model_.ReservationPlan().PriorityOrder()) {
        IoRequest* const request = frames_.Io(frame);
        if (!request || request->header_submitted ||
            request->content_submitted) {
            continue;
        }
        synchronous_progress =
            SubmitPreparedRead(frame) || synchronous_progress;
    }
    return synchronous_progress;
}

bool StoragePipeline::SubmitEligibleReads() {
    if (!PrepareEligibleReads()) return false;
    return SubmitPreparedReads();
}

}  // namespace pv
