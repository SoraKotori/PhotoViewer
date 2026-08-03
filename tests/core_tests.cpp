#include "navigation.h"
#include "png.h"
#include "resource_slots.h"

#include <array>
#include <cstdlib>
#include <iostream>

namespace {

void Check(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

std::size_t PresentNext(pv::NavigationState& navigation) {
    const auto next = navigation.NextIndex();
    Check(next.has_value(), "expected a pending presentation");
    navigation.CompletePresentation(*next);
    return *next;
}

void NavigationTests() {
    pv::NavigationState navigation;
    navigation.Reset(5, 20);
    Check(PresentNext(navigation) == 5, "initial image must be presented first");

    for (int count = 0; count < 5; ++count) navigation.Step(1, false);
    navigation.Step(-1, false);
    const std::array<std::size_t, 6> expected{6, 7, 8, 9, 10, 9};
    for (const std::size_t index : expected) {
        Check(PresentNext(navigation) == index, "short presses must preserve order");
    }
    Check(navigation.Empty(), "short press sequence must drain exactly");

    navigation.Reset(5, 20);
    PresentNext(navigation);
    navigation.Step(1, false);
    navigation.Step(1, true);
    navigation.Step(1, true);
    navigation.Release(1);
    Check(PresentNext(navigation) == 6, "release must retain the initial committed press");
    Check(navigation.Empty(), "release must discard unpresented repeats");

    navigation.Reset(0, 2);
    PresentNext(navigation);
    navigation.Step(-1, false);
    Check(navigation.Empty(), "navigation must stop at folder boundary");
    navigation.Step(1, false);
    Check(PresentNext(navigation) == 1, "forward boundary step");
    navigation.Step(1, false);
    Check(navigation.Empty(), "upper boundary must not wrap");

    navigation.Reset(10, 30);
    PresentNext(navigation);
    navigation.Step(1, false);
    navigation.Step(1, true);
    navigation.Step(-1, false);
    Check(PresentNext(navigation) == 11, "committed forward step survives reversal");
    Check(PresentNext(navigation) == 10, "committed reverse step follows in order");
    Check(navigation.Empty(), "old-direction repeat is discarded on reversal");

    navigation.Reset(10, 30);
    PresentNext(navigation);
    navigation.Step(-1, false);
    navigation.Release(-1);
    Check(PresentNext(navigation) == 9, "left short press must be presented");
    Check(navigation.Empty(), "left short press must drain exactly");
    Check(navigation.PreferredDirection() == -1,
          "idle prefetch must retain the last successful left direction");
    navigation.Step(1, false);
    navigation.Release(1);
    Check(PresentNext(navigation) == 10, "right short press after left must be presented");
    Check(navigation.PreferredDirection() == 1,
          "idle prefetch must retain the last successful right direction");

    navigation.Reset(0, 30);
    PresentNext(navigation);
    navigation.Step(-1, false);
    navigation.Release(-1);
    Check(navigation.PreferredDirection() == 0,
          "a rejected boundary step must not replace the last effective direction");
}

void PngTests() {
    std::array<std::byte, 33> header{};
    const std::array<unsigned char, 24> prefix{
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,
        0x00, 0x00, 0x00, 0x0D, 'I', 'H', 'D', 'R',
        0x00, 0x00, 0x1E, 0x00, 0x00, 0x00, 0x10, 0xE0};
    for (std::size_t index = 0; index < prefix.size(); ++index) {
        header[index] = std::byte{prefix[index]};
    }
    const auto parsed = pv::ParsePngHeader(header);
    Check(parsed.has_value(), "valid PNG IHDR");
    Check(parsed->width == 7680 && parsed->height == 4320, "8K dimensions");
    Check(parsed->decoded_bytes == 7680ULL * 4320ULL * 4ULL,
          "decoded byte reservation");
    header[0] = std::byte{0};
    Check(!pv::ParsePngHeader(header), "invalid signature must fail");
}

void ResourceSlotTests() {
    pv::ResourceSlots slots(2, 2, 2, 8192, 8192);
    Check(slots.CompressedCount() == 2 && slots.FreeCompressedCount() == 2,
          "compressed slot storage and free index must start at configured count");
    Check(slots.CpuSurfaceCount() == 2 && slots.FreeCpuSurfaceCount() == 2,
          "CPU slot storage and free index must start at configured count");
    Check(slots.GpuTextureCount() == 2 && slots.FreeGpuTextureCount() == 2,
          "GPU slot storage and free index must start at configured count");

    const pv::SlotId compressed0 = slots.AcquireCompressed(4096, 3, 7);
    const pv::SlotId compressed1 = slots.AcquireCompressed(4096, 4, 7);
    Check(compressed0 != pv::kInvalidSlot && compressed1 != pv::kInvalidSlot,
          "configured compressed slots must be allocatable");
    Check(slots.FreeCompressedCount() == 0,
          "compressed free index must exclude occupied slots");
    Check(slots.AcquireCompressed(1, 5, 7) == pv::kInvalidSlot,
          "compressed slot count must be a hard limit");
    Check(slots.Compressed(compressed0).state ==
              pv::CompressedSlotState::FileReadDestination,
          "compressed acquisition state must describe its pipeline phase");
    slots.Compressed(compressed0).state =
        pv::CompressedSlotState::CompressedDataAvailable;
    slots.ReleaseCompressed(compressed0);
    Check(slots.FreeCompressedCount() == 1 &&
              slots.Compressed(compressed0).state == pv::CompressedSlotState::Free,
          "compressed release must restore state and free index together");

    const pv::SlotId cpu0 = slots.AcquireCpuSurface(4096, 3, 7);
    const pv::SlotId cpu1 = slots.AcquireCpuSurface(4096, 4, 7);
    Check(cpu0 != pv::kInvalidSlot && cpu1 != pv::kInvalidSlot,
          "configured CPU surface slots must be allocatable");
    Check(slots.AcquireCpuSurface(1, 5, 7) == pv::kInvalidSlot,
          "CPU surface slot count must be a hard limit");
    Check(slots.CpuSurfaceAt(cpu0).state == pv::CpuSurfaceSlotState::DecodeOutput,
          "CPU acquisition state must describe its pipeline phase");
    slots.CpuSurfaceAt(cpu0).state =
        pv::CpuSurfaceSlotState::DecodedPixelsAvailable;
    slots.ReleaseCpuSurface(cpu0);
    Check(slots.FreeCpuSurfaceCount() == 1 &&
              slots.CpuSurfaceAt(cpu0).state == pv::CpuSurfaceSlotState::Free,
          "CPU release must restore state and free index together");

    const pv::SlotId gpu0 = slots.AcquireGpuTexture(3, 7);
    const pv::SlotId gpu1 = slots.AcquireGpuTexture(4, 7);
    Check(gpu0 != pv::kInvalidSlot && gpu1 != pv::kInvalidSlot,
          "configured GPU texture slots must be allocatable");
    Check(slots.AcquireGpuTexture(5, 7) == pv::kInvalidSlot,
          "GPU texture slot count must be a hard limit");
    Check(slots.GpuTextureAt(gpu0).state ==
              pv::GpuTextureSlotState::UploadDestination,
          "GPU acquisition state must describe its pipeline phase");
    slots.GpuTextureAt(gpu0).state = pv::GpuTextureSlotState::Presentable;
    slots.ReleaseGpuTexture(gpu0);
    Check(slots.FreeGpuTextureCount() == 1 &&
              slots.GpuTextureAt(gpu0).state == pv::GpuTextureSlotState::Free,
          "GPU release must restore state and free index together");

    pv::ResourceSlots budget_limited(2, 2, 1, 4096, 4096);
    Check(budget_limited.AcquireCompressed(4096, 0, 1) != pv::kInvalidSlot,
          "compressed byte budget must allow one exact allocation");
    Check(budget_limited.AcquireCompressed(1, 1, 1) == pv::kInvalidSlot,
          "compressed byte budget and slot count must both be enforced");
    Check(budget_limited.AcquireCpuSurface(4096, 0, 1) != pv::kInvalidSlot,
          "CPU byte budget must allow one exact allocation");
    Check(budget_limited.AcquireCpuSurface(1, 1, 1) == pv::kInvalidSlot,
          "CPU byte budget and slot count must both be enforced");
}

}  // namespace

int main() {
    NavigationTests();
    PngTests();
    ResourceSlotTests();
    std::cout << "PASS: core tests\n";
    return 0;
}
