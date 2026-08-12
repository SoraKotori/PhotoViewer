#include "upload_ledger.h"

#include <stdexcept>

namespace pv {

UploadLedger::UploadLedger(const std::size_t capacity) : capacity_(capacity) {
    tickets_.reserve(capacity);
}

void UploadLedger::Queue(UploadTicket ticket) {
    if (ticket.fence_value == 0 ||
        (!tickets_.empty() &&
         ticket.fence_value < tickets_.back().fence_value)) {
        throw std::logic_error("GPU upload fences must be monotonic");
    }
    if (tickets_.size() == capacity_) {
        throw std::logic_error("GPU upload ledger capacity exceeded");
    }
    tickets_.push_back(std::move(ticket));
}

std::optional<UploadTicket> UploadLedger::TakeCompleted(
    const std::uint64_t completed_fence) {
    if (tickets_.empty() || tickets_.front().fence_value > completed_fence) {
        return std::nullopt;
    }
    UploadTicket ticket = std::move(tickets_.front());
    tickets_.erase(tickets_.begin());
    return ticket;
}

std::uint64_t UploadLedger::OldestFence() const noexcept {
    return tickets_.empty() ? 0 : tickets_.front().fence_value;
}

}  // namespace pv
