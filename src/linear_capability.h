#pragma once

#include <stdexcept>
#include <utility>

namespace pv {

// Move-only authorization for a borrowed target. Moving transfers the
// authorization and revokes the source; it never owns or extends the target's
// lifetime.
template <typename TargetType>
class LinearCapability {
public:
    LinearCapability(const LinearCapability&) = delete;
    LinearCapability& operator=(const LinearCapability&) = delete;

    LinearCapability(LinearCapability&& other) noexcept
        : target_(std::exchange(other.target_, nullptr)) {}
    LinearCapability& operator=(LinearCapability&&) = delete;

    [[nodiscard]] bool IsValid() const noexcept { return target_ != nullptr; }

protected:
    explicit LinearCapability(TargetType& target) noexcept : target_(&target) {}

    [[nodiscard]] TargetType& Target() const {
        if (!target_) {
            throw std::logic_error("capability used after transfer");
        }
        return *target_;
    }

private:
    TargetType* target_;
};

}  // namespace pv
