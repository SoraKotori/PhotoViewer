#include "app.h"

#include "common.h"

namespace pv {
namespace {

DWORD QueryIoPrefixGranularity(const HANDLE file) noexcept {
    SYSTEM_INFO system{};
    GetNativeSystemInfo(&system);
    DWORD granularity = std::max<DWORD>(4096, system.dwPageSize);
    FILE_STORAGE_INFO storage{};
    if (GetFileInformationByHandleEx(file, FileStorageInfo, &storage,
                                     sizeof(storage))) {
        granularity = std::max({granularity,
                                storage.LogicalBytesPerSector,
                                storage.PhysicalBytesPerSectorForAtomicity,
                                storage.PhysicalBytesPerSectorForPerformance,
                                storage.FileSystemEffectivePhysicalBytesPerSectorForAtomicity});
    }
    return granularity;
}

}  // namespace

bool PipelineRuntime::DrainCompletions() {
    bool drained = false;
    std::array<OVERLAPPED_ENTRY, 32> entries{};
    if (!ResetEvent(io_completion_event_.Get())) {
        ThrowLastError("ResetEvent(IOCP)");
    }
    for (;;) {
        ULONG removed = 0;
        if (!GetQueuedCompletionStatusEx(
                io_completion_port_.Get(), entries.data(),
                static_cast<ULONG>(entries.size()), &removed, 0, FALSE)) {
            const DWORD result = GetLastError();
            if (result != WAIT_TIMEOUT) {
                SetLastError(result);
                ThrowLastError("GetQueuedCompletionStatusEx");
            }
            break;
        }
        for (ULONG index = 0; index < removed; ++index) {
            const OVERLAPPED_ENTRY& entry = entries[index];
            auto* const request = reinterpret_cast<IoRequest*>(
                entry.lpCompletionKey);
            DWORD transferred = entry.dwNumberOfBytesTransferred;
            DWORD result = ERROR_SUCCESS;
            if (entry.Internal != 0) {
                if (!request || !request->file ||
                    !GetOverlappedResult(request->file.Get(), entry.lpOverlapped,
                                         &transferred, FALSE)) {
                    result = GetLastError();
                }
            }
            drained = true;
            OnIoCompletion(request, entry.lpOverlapped, result, transferred);
        }
    }
    return drained;
}

void PipelineRuntime::OnIoCompletion(IoRequest* const request,
                         OVERLAPPED* const overlapped,
                         const DWORD result,
                         const ULONG_PTR transferred) {
    if (!request || !overlapped || !request->file) return;
    if (overlapped == &request->header_overlapped) {
        OnIoHeaderReady(request, result, transferred);
    } else if (overlapped == &request->content_overlapped) {
        OnIoComplete(request, result, transferred);
    }
}

void PipelineRuntime::OnIoHeaderReady(IoRequest* request,
                          const DWORD result,
                          const ULONG_PTR transferred) {
    if (!request || request->index >= state_.images.size()) return;
    ImageRecord& image = state_.images[request->index];
    if (!image.io || image.io != request ||
        request->generation != image.generation || request->header_completed) {
        return;
    }
    request->header_result = result;
    request->header_transferred = transferred;
    request->header_completed = true;
    if (request->index == state_.navigation.CurrentIndex() &&
        state_.navigation.InitialPending() &&
        validation_.At(StartupMilestone::InitialHeaderReady) ==
            std::chrono::steady_clock::time_point{}) {
        validation_.Mark(StartupMilestone::InitialHeaderReady);
    }
    if (request->header_result == ERROR_SUCCESS && transferred >= 24) {
        const auto header = ParsePngHeader(std::span<const std::byte>(
            request->destination, 24));
        if (header) {
            CatalogItem& item = state_.catalog.items[request->index];
            item.png = *header;
            item.header_valid = true;
            PrepareStagingForImage(request->index);
            if (!graphics_device_ready_ &&
                state_.navigation.InitialPending() &&
                request->index == state_.navigation.CurrentIndex() &&
                image.staging_slot != kInvalidSlot) {
                DecodeStaging& staging = state_.slots.StagingAt(
                    image.staging_slot).resource;
                (void)staging.PrepareCpuSurface(
                    item.png.width, item.png.height, item.png.decoded_bytes);
            }
        }
    }
    if (!request->content_submitted || request->content_completed) {
        if (!request->content_submitted) {
            request->result = ERROR_SUCCESS;
            request->transferred = 0;
        }
        CompleteIoRequest(request);
    }
}

void PipelineRuntime::OnIoComplete(IoRequest* request, const DWORD result,
                       const ULONG_PTR transferred) {
    if (request && request->index < state_.images.size()) {
        ImageRecord& image = state_.images[request->index];
        if (image.io == request && !request->content_completed) {
            if (request->result == ERROR_IO_PENDING || result != ERROR_SUCCESS) {
                request->result = result;
            }
            request->transferred += transferred;
            request->content_completed = true;
            if (request->index == state_.navigation.CurrentIndex() &&
                state_.navigation.InitialPending() &&
                validation_.At(StartupMilestone::InitialContentCompletionObserved) ==
                    std::chrono::steady_clock::time_point{}) {
                // IOCP packets do not include a kernel completion timestamp.
                // Synchronous success is observed here immediately; pending
                // I/O is observed when main dequeues its completion packet.
                validation_.Mark(
                    StartupMilestone::InitialContentCompletionObserved);
            }
            if (!request->split_header || request->header_completed) {
                CompleteIoRequest(request);
            }
        }
    }
}

void PipelineRuntime::CompleteIoRequest(IoRequest* const request) {
    if (!request || request->index >= state_.images.size()) return;
    ImageRecord& image = state_.images[request->index];
    if (!image.io || image.io != request) return;

    request->file.Reset();

    const DWORD io_result = request->result;
    const std::size_t transferred = request->transferred;
    const SlotId compressed_slot = request->compressed_slot;
    CompressedSlot& slot = state_.slots.Compressed(compressed_slot);
    const std::size_t allocation = slot.resource.size;
    const bool header_success = !request->split_header ||
        (request->header_result == ERROR_SUCCESS &&
         request->header_transferred == request->prefix_bytes);
    const std::size_t expected_content = request->split_header
                                             ? allocation - request->prefix_bytes
                                             : allocation;
    const bool success = header_success && io_result == ERROR_SUCCESS &&
                         transferred == expected_content;
    const bool current = request->generation == image.generation;
    image.io = nullptr;
    if (request->index == state_.navigation.CurrentIndex() &&
        state_.navigation.InitialPending() &&
        validation_.At(StartupMilestone::InitialContentReady) ==
            std::chrono::steady_clock::time_point{}) {
        validation_.Mark(StartupMilestone::InitialContentReady);
    }

    const bool reserved = current && ReservationActive(
        state_.reservations.Compressed(), image.compressed_reservation,
        request->index);
    if (success && reserved) {
        CatalogItem& item = state_.catalog.items[request->index];
        const auto header = ParsePngHeader(std::span<const std::byte>(
            slot.resource.data, slot.resource.size));
        if (header) {
            item.png = *header;
            item.header_valid = true;
            slot.state = CompressedSlotState::CompressedDataAvailable;
        } else {
            if (state_.compressed_bytes >= allocation) {
                state_.compressed_bytes -= allocation;
            }
            state_.slots.ReleaseCompressed(compressed_slot);
            image.compressed_slot = kInvalidSlot;
            image.failed = true;
        }
    } else {
        if (state_.compressed_bytes >= allocation) state_.compressed_bytes -= allocation;
        state_.slots.ReleaseCompressed(compressed_slot);
        image.compressed_slot = kInvalidSlot;
        if (reserved && io_result != ERROR_OPERATION_ABORTED) {
            image.failed = true;
        }
    }
}

void PipelineRuntime::SubmitReads() {
    struct ReadSubmission {
        DWORD result = ERROR_SUCCESS;
        DWORD transferred = 0;
        bool completed = false;
    };
    const auto submit_read = [&](IoRequest& request, OVERLAPPED& overlapped,
                                 const DWORD buffer_offset, const DWORD bytes,
                                 const std::uint64_t file_offset) {
        overlapped = {};
        overlapped.hEvent = io_completion_event_.Get();
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
    const auto submit_request = [&](ImageRecord& image) {
        IoRequest* const request = image.io;
        ReadSubmission header;
        ReadSubmission content;
        if (request->split_header) {
            request->prefix_bytes = std::min(
                request->byte_count, io_prefix_granularity_);
            const DWORD prefix_transfer = std::min(
                request->transfer_count, io_prefix_granularity_);
            request->header_submitted = true;
            header = validation_.Measure(
                TimedOperation::SubmitFileReads,
                [&] {
                    return submit_read(*request, request->header_overlapped,
                                       0, prefix_transfer, 0);
                });
            if (prefix_transfer < request->transfer_count) {
                request->result = ERROR_SUCCESS;
                request->content_submitted = true;
                content = validation_.Measure(
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
            content = validation_.Measure(
                TimedOperation::SubmitFileReads,
                [&] {
                    return submit_read(*request, request->content_overlapped,
                                       0, request->transfer_count, 0);
                });
        }
        if (header.completed && image.io == request) {
            OnIoHeaderReady(request, header.result, header.transferred);
        }
        if (content.completed && image.io == request) {
            OnIoComplete(request, content.result, content.transferred);
        }
    };
    const bool initial_content_pending =
        state_.navigation.InitialPending() &&
        validation_.At(StartupMilestone::InitialContentReady) ==
            std::chrono::steady_clock::time_point{};
    const std::size_t initial_index = state_.navigation.CurrentIndex();
    bool prepared_reads = false;
    for (const std::size_t index : state_.reservations.PriorityOrder()) {
        if (initial_content_pending && index != initial_index) continue;
        if (StageOf(state_.images[index]) != PipelineStage::WaitingIo) {
            continue;
        }
        ImageRecord& image = state_.images[index];
        CatalogItem& item = state_.catalog.items[index];
        HANDLE opened_file = validation_.Measure(
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
            image.failed = true;
            continue;
        }
        if (!item.file_size_known) {
            LARGE_INTEGER file_size{};
            if (!GetFileSizeEx(opened_file, &file_size) ||
                file_size.QuadPart <= 0 ||
                static_cast<unsigned long long>(file_size.QuadPart) >
                    std::numeric_limits<DWORD>::max()) {
                CloseHandle(opened_file);
                image.failed = true;
                continue;
            }
            item.file_bytes = static_cast<std::uint64_t>(file_size.QuadPart);
            item.file_size_known = true;
        }
        if (item.file_bytes <= 24 ||
            item.file_bytes > std::numeric_limits<DWORD>::max()) {
            CloseHandle(opened_file);
            image.failed = true;
            continue;
        }
        const std::size_t compressed = static_cast<std::size_t>(item.file_bytes);
        if (compressed > config_.compressed_budget_bytes) {
            CloseHandle(opened_file);
            image.failed = true;
            continue;
        }
        if (state_.compressed_bytes >
                config_.compressed_budget_bytes - compressed) {
            CloseHandle(opened_file);
            continue;
        }
        if (io_prefix_granularity_ == 0) {
            io_prefix_granularity_ = QueryIoPrefixGranularity(opened_file);
        }
        const std::size_t granularity = io_prefix_granularity_;
        if (granularity == 0 ||
            compressed > std::numeric_limits<DWORD>::max() - (granularity - 1)) {
            CloseHandle(opened_file);
            image.failed = true;
            continue;
        }
        const std::size_t transfer_bytes =
            ((compressed + granularity - 1) / granularity) * granularity;
        const SlotId compressed_slot = validation_.Measure(
            TimedOperation::AcquireCompressed,
            [&] {
                return state_.slots.AcquireCompressed(
                    transfer_bytes, index, image.generation);
            });
        if (compressed_slot == kInvalidSlot) {
            CloseHandle(opened_file);
            continue;
        }
        image.compressed_slot = compressed_slot;
        CompressedSlot& compressed_state =
            state_.slots.Compressed(compressed_slot);
        IoRequest& request = compressed_state.io;
        request.Reset();
        request.index = index;
        request.generation = image.generation;
        request.compressed_slot = compressed_slot;
        request.file.Reset(opened_file);

        state_.compressed_bytes += compressed;
        image.io = &request;
        CompressedBuffer& buffer = compressed_state.resource;
        buffer.size = compressed;
        image.io->destination = buffer.data;
        image.io->byte_count = static_cast<DWORD>(compressed);
        image.io->transfer_count = static_cast<DWORD>(transfer_bytes);
        image.io->split_header = !item.header_valid;
        const HANDLE associated = validation_.Measure(
            TimedOperation::SubmitFileReads,
            [&] {
                return CreateIoCompletionPort(
                    opened_file, io_completion_port_.Get(),
                    reinterpret_cast<ULONG_PTR>(image.io), 0);
            });
        if (associated != io_completion_port_.Get()) {
            ThrowLastError("Associate file with IOCP");
        }
        if (!SetFileCompletionNotificationModes(
                opened_file, FILE_SKIP_SET_EVENT_ON_HANDLE |
                                 FILE_SKIP_COMPLETION_PORT_ON_SUCCESS)) {
            ThrowLastError("SetFileCompletionNotificationModes");
        }
        prepared_reads = true;
    }
    if (!prepared_reads) return;

    for (const std::size_t index : state_.reservations.PriorityOrder()) {
        ImageRecord& image = state_.images[index];
        if (!image.io || image.io->header_submitted ||
            image.io->content_submitted) {
            continue;
        }
        submit_request(image);
    }
}

void PipelineRuntime::CancelAllIo() {
    std::size_t pending_operations = 0;
    for (ImageRecord& image : state_.images) {
        if (image.io && image.io->file) {
            CancelIoEx(image.io->file.Get(), nullptr);
            if (image.io->header_submitted && !image.io->header_completed) {
                ++pending_operations;
            }
            if (image.io->content_submitted && !image.io->content_completed) {
                ++pending_operations;
            }
        }
    }
    while (pending_operations != 0) {
        DWORD transferred = 0;
        ULONG_PTR completion_key = 0;
        OVERLAPPED* overlapped = nullptr;
        const BOOL completed = GetQueuedCompletionStatus(
            io_completion_port_.Get(), &transferred, &completion_key, &overlapped,
            INFINITE);
        if (!overlapped) {
            if (!completed) ThrowLastError("GetQueuedCompletionStatus(shutdown)");
            throw std::runtime_error("IOCP returned an empty shutdown completion");
        }
        auto* const request = reinterpret_cast<IoRequest*>(completion_key);
        if (overlapped == &request->header_overlapped &&
            request->header_submitted && !request->header_completed) {
            request->header_completed = true;
            --pending_operations;
        } else if (overlapped == &request->content_overlapped &&
                   request->content_submitted &&
                   !request->content_completed) {
            request->content_completed = true;
            --pending_operations;
        }
    }
    for (ImageRecord& image : state_.images) {
        if (!image.io) continue;
        image.io->file.Reset();
        if (image.io->compressed_slot != kInvalidSlot) {
            const std::size_t bytes = state_.slots.Compressed(
                image.io->compressed_slot).resource.size;
            if (state_.compressed_bytes >= bytes) state_.compressed_bytes -= bytes;
            state_.slots.ReleaseCompressed(image.io->compressed_slot);
            image.compressed_slot = kInvalidSlot;
        }
        image.io = nullptr;
    }
}

}  // namespace pv



