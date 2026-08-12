#include "iocp_transport.h"

#include "win32_support.h"

#include <algorithm>
#include <array>

namespace pv {
namespace {

DWORD QueryTransferGranularity(const HANDLE file) noexcept {
    SYSTEM_INFO system{};
    GetNativeSystemInfo(&system);
    DWORD granularity = std::max<DWORD>(4096, system.dwPageSize);
    FILE_STORAGE_INFO storage{};
    if (GetFileInformationByHandleEx(file, FileStorageInfo, &storage,
                                     sizeof(storage))) {
        granularity = std::max({
            granularity, storage.LogicalBytesPerSector,
            storage.PhysicalBytesPerSectorForAtomicity,
            storage.PhysicalBytesPerSectorForPerformance,
            storage.FileSystemEffectivePhysicalBytesPerSectorForAtomicity});
    }
    return granularity;
}

IoCompletion ResolveCompletion(const OVERLAPPED_ENTRY& entry) noexcept {
    IoCompletion completion;
    completion.request = reinterpret_cast<IoRequest*>(entry.lpCompletionKey);
    completion.overlapped = entry.lpOverlapped;
    completion.transferred = entry.dwNumberOfBytesTransferred;
    if (entry.Internal != 0 &&
        (!completion.request || !completion.request->file ||
         !GetOverlappedResult(completion.request->file.Get(),
                              completion.overlapped,
                              &completion.transferred, FALSE))) {
        completion.result = GetLastError();
    }
    return completion;
}

}  // namespace

IocpTransport::IocpTransport() {
    completion_port_.Reset(CreateIoCompletionPort(
        INVALID_HANDLE_VALUE, nullptr, 0, 1));
    if (!completion_port_) ThrowLastError("CreateIoCompletionPort");
    completion_event_.Reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!completion_event_) ThrowLastError("CreateEventW(IOCP)");
}

HANDLE IocpTransport::CompletionEvent() const noexcept {
    return completion_event_.Get();
}

bool IocpTransport::Enabled() const noexcept {
    return static_cast<bool>(completion_port_);
}

DWORD IocpTransport::TransferGranularity(const HANDLE file) noexcept {
    if (transfer_granularity_ == 0) {
        transfer_granularity_ = QueryTransferGranularity(file);
    }
    return transfer_granularity_;
}

DWORD IocpTransport::CachedTransferGranularity() const noexcept {
    return transfer_granularity_;
}

std::size_t IocpTransport::DrainAvailable(
    const std::span<IoCompletion> completions) {
    if (completions.empty()) return 0;
    if (!ResetEvent(completion_event_.Get())) ThrowLastError("ResetEvent(IOCP)");

    std::array<OVERLAPPED_ENTRY, 32> entries{};
    const ULONG capacity = static_cast<ULONG>(
        std::min(completions.size(), entries.size()));
    ULONG removed = 0;
    if (!GetQueuedCompletionStatusEx(completion_port_.Get(), entries.data(),
                                     capacity, &removed, 0, FALSE)) {
        const DWORD error = GetLastError();
        if (error == WAIT_TIMEOUT) return 0;
        SetLastError(error);
        ThrowLastError("GetQueuedCompletionStatusEx");
    }
    for (ULONG index = 0; index < removed; ++index) {
        completions[index] = ResolveCompletion(entries[index]);
    }
    return removed;
}

IoCompletion IocpTransport::WaitForShutdownCompletion() {
    DWORD transferred = 0;
    ULONG_PTR completion_key = 0;
    OVERLAPPED* overlapped = nullptr;
    const BOOL completed = GetQueuedCompletionStatus(
        completion_port_.Get(), &transferred, &completion_key, &overlapped,
        INFINITE);
    if (!overlapped) {
        if (!completed) ThrowLastError("GetQueuedCompletionStatus(shutdown)");
        throw std::runtime_error("IOCP returned an empty shutdown completion");
    }
    OVERLAPPED_ENTRY entry{};
    entry.lpCompletionKey = completion_key;
    entry.lpOverlapped = overlapped;
    entry.dwNumberOfBytesTransferred = transferred;
    entry.Internal = completed ? 0 : 1;
    return ResolveCompletion(entry);
}

void IocpTransport::Associate(const HANDLE file, IoRequest& request) {
    const HANDLE associated = CreateIoCompletionPort(
        file, completion_port_.Get(), reinterpret_cast<ULONG_PTR>(&request), 0);
    if (associated != completion_port_.Get()) {
        ThrowLastError("Associate file with IOCP");
    }
}

void IocpTransport::ConfigureCompletionMode(const HANDLE file) {
    if (!SetFileCompletionNotificationModes(
            file, FILE_SKIP_SET_EVENT_ON_HANDLE |
                      FILE_SKIP_COMPLETION_PORT_ON_SUCCESS)) {
        ThrowLastError("SetFileCompletionNotificationModes");
    }
}

bool IocpTransport::RequestCancellation(IoRequest& request) {
    if (!request.file) return false;
    if (CancelIoEx(request.file.Get(), nullptr)) return true;
    const DWORD error = GetLastError();
    if (error == ERROR_NOT_FOUND) return false;
    SetLastError(error);
    ThrowLastError("CancelIoEx");
}

}  // namespace pv
