#pragma once

#include "pipeline_types.h"

#include <optional>
#include <vector>

namespace pv {

// D3D immediate-context submissions produce monotonically increasing fences.
// A completion value may skip notifications; every ticket at or below that
// value is complete regardless of its frame index.
class UploadLedger {
public:
    explicit UploadLedger(std::size_t capacity);
    void Queue(UploadTicket ticket);
    [[nodiscard]] std::optional<UploadTicket> TakeCompleted(
        std::uint64_t completed_fence);
    [[nodiscard]] std::uint64_t OldestFence() const noexcept;
    [[nodiscard]] bool Empty() const noexcept { return tickets_.empty(); }
    [[nodiscard]] std::size_t Count() const noexcept { return tickets_.size(); }

private:
    const std::size_t capacity_;
    std::vector<UploadTicket> tickets_;
};

}  // namespace pv
