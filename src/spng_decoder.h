#pragma once

#include "model.h"

namespace pv {

using InputConsumedCallback = void (*)(void*) noexcept;

HRESULT DecodePngSpng(std::span<std::byte> compressed,
                      CpuSurface& surface,
                      InputConsumedCallback input_consumed = nullptr,
                      void* callback_context = nullptr) noexcept;

}  // namespace pv
