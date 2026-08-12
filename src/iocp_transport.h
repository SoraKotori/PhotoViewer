#pragma once

#include "io_request.h"
#include "win32_handle.h"

#include <cstddef>
#include <span>

namespace pv {

struct IoCompletion {
    IoRequest* request = nullptr;
    OVERLAPPED* overlapped = nullptr;
    DWORD result = ERROR_SUCCESS;
    DWORD transferred = 0;
};

// Owns the Windows asynchronous file-I/O transport. The pipeline sees only
// completed value records and never manipulates the completion port itself.
class IocpTransport {
public:
    IocpTransport();
    IocpTransport(const IocpTransport&) = delete;
    IocpTransport& operator=(const IocpTransport&) = delete;

    [[nodiscard]] HANDLE CompletionEvent() const noexcept;
    [[nodiscard]] bool Enabled() const noexcept;
    [[nodiscard]] DWORD TransferGranularity(HANDLE file) noexcept;
    [[nodiscard]] DWORD CachedTransferGranularity() const noexcept;

    std::size_t DrainAvailable(std::span<IoCompletion> completions);
    [[nodiscard]] IoCompletion WaitForShutdownCompletion();
    void Associate(HANDLE file, IoRequest& request);
    void ConfigureCompletionMode(HANDLE file);
    [[nodiscard]] bool RequestCancellation(IoRequest& request);

private:
    UniqueHandle completion_port_;
    UniqueHandle completion_event_;
    DWORD transfer_granularity_ = 0;
};

}  // namespace pv
