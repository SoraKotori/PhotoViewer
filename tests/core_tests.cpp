#include "navigation.h"
#include "png.h"

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

}  // namespace

int main() {
    NavigationTests();
    PngTests();
    std::cout << "PASS: core tests\n";
    return 0;
}
