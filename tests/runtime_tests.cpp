#include "common.h"
#include "decoder.h"
#include "fast_png.h"
#include "graphics.h"
#include "wuffs_png.h"

#include <array>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <latch>

namespace {

void Check(const bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void TestWic() {
    const std::array<unsigned char, 70> raw{
        0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A,
        0x00,0x00,0x00,0x0D,0x49,0x48,0x44,0x52,
        0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,
        0x08,0x06,0x00,0x00,0x00,0x1F,0x15,0xC4,0x89,
        0x00,0x00,0x00,0x0D,0x49,0x44,0x41,0x54,
        0x08,0xD7,0x63,0xF8,0xCF,0xC0,0xF0,0x1F,0x00,
        0x05,0x00,0x01,0xFF,0x89,0x99,0x3D,0x1D,
        0x00,0x00,0x00,0x00,0x49,0x45,0x4E,0x44,
        0xAE,0x42,0x60,0x82};
    std::array<std::byte, raw.size()> png{};
    for (std::size_t index = 0; index < raw.size(); ++index) png[index] = std::byte{raw[index]};

    pv::ComPtr<IWICImagingFactory> factory;
    pv::CheckHr(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                 IID_PPV_ARGS(&factory)), "Create WIC factory");
    pv::CpuSurface surface;
    surface.width = 1;
    surface.height = 1;
    surface.stride = 4;
    Check(surface.Allocate(4), "allocate decoded surface");
    pv::CheckHr(pv::DecodePngMemory(factory.Get(), png, surface), "Decode embedded PNG");
    Check(surface.pixels[3] == std::byte{0xFF}, "decoded alpha channel");
}

void TestGraphics(const HINSTANCE instance) {
    constexpr wchar_t class_name[] = L"PhotoViewer.RuntimeTest";
    WNDCLASSW window_class{};
    window_class.lpfnWndProc = DefWindowProcW;
    window_class.hInstance = instance;
    window_class.lpszClassName = class_name;
    Check(RegisterClassW(&window_class) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS,
          "Register runtime test window");
    HWND window = CreateWindowExW(0, class_name, L"runtime-test", WS_OVERLAPPEDWINDOW,
                                  0, 0, 320, 240, nullptr, nullptr, instance, nullptr);
    Check(window != nullptr, "Create runtime test window");

    {
        pv::Graphics graphics;
        graphics.Initialize(window);
        auto surface = std::make_unique<pv::CpuSurface>();
        surface->width = 64;
        surface->height = 64;
        surface->stride = 64 * 4;
        Check(surface->Allocate(64 * 64 * 4), "allocate upload surface");
        for (std::size_t offset = 0; offset < surface->ByteSize(); offset += 4) {
            surface->pixels[offset + 0] = std::byte{0x20};
            surface->pixels[offset + 1] = std::byte{0x80};
            surface->pixels[offset + 2] = std::byte{0xE0};
            surface->pixels[offset + 3] = std::byte{0xFF};
        }
        pv::UploadTicket ticket = graphics.SubmitUpload(0, 1, std::move(surface));
        graphics.ArmFence(ticket.fence_value);
        Check(WaitForSingleObject(graphics.FenceEvent(), 5000) == WAIT_OBJECT_0,
              "D3D11 fence completion");
        pv::GpuImage image = graphics.FinishUpload(ticket);
        Check(WaitForSingleObject(graphics.FrameWaitableObject(), 5000) == WAIT_OBJECT_0,
              "initial frame credit");
        graphics.Draw(image);
        graphics.Resize(400, 300);
        Check(WaitForSingleObject(graphics.FrameWaitableObject(), 5000) == WAIT_OBJECT_0,
              "resized frame credit");
        graphics.Draw(image);
    }
    DestroyWindow(window);
}

std::uint32_t ReadBigEndian(const std::byte* const data) {
    return (static_cast<std::uint32_t>(data[0]) << 24U) |
           (static_cast<std::uint32_t>(data[1]) << 16U) |
           (static_cast<std::uint32_t>(data[2]) << 8U) |
           static_cast<std::uint32_t>(data[3]);
}

int BenchmarkDecode(const std::filesystem::path& path, const std::size_t workers,
                    const bool fast) {
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(path.parent_path())) {
        if (entry.is_regular_file() && entry.path().extension() == L".png") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    const auto first = std::find(files.begin(), files.end(), path);
    Check(first != files.end() && static_cast<std::size_t>(files.end() - first) >= workers,
          "not enough benchmark PNGs after starting image");

    std::vector<std::vector<std::byte>> compressed_images;
    compressed_images.reserve(workers);
    const auto load_begin = std::chrono::steady_clock::now();
    std::size_t compressed_bytes = 0;

    std::vector<std::unique_ptr<pv::CpuSurface>> surfaces;
    surfaces.reserve(workers);
    for (std::size_t index = 0; index < workers; ++index) {
        std::ifstream input(*(first + index), std::ios::binary | std::ios::ate);
        Check(input.good(), "open benchmark PNG");
        const auto length = input.tellg();
        Check(length >= 24, "benchmark PNG is too small");
        std::vector<std::byte> compressed(static_cast<std::size_t>(length));
        input.seekg(0);
        input.read(reinterpret_cast<char*>(compressed.data()), length);
        Check(input.good(), "read benchmark PNG");
        compressed_bytes += compressed.size();
        const std::uint32_t width = ReadBigEndian(compressed.data() + 16);
        const std::uint32_t height = ReadBigEndian(compressed.data() + 20);
        const std::size_t decoded_bytes = static_cast<std::size_t>(width) * height * 4;
        auto surface = std::make_unique<pv::CpuSurface>();
        surface->width = width;
        surface->height = height;
        surface->stride = width * 4;
        Check(surface->Allocate(decoded_bytes + (fast ? height : 0)),
              "allocate benchmark surface");
        surface->byte_size = decoded_bytes;
        compressed_images.push_back(std::move(compressed));
        surfaces.push_back(std::move(surface));
    }
    const auto load_elapsed = std::chrono::steady_clock::now() - load_begin;

    std::latch ready(static_cast<std::ptrdiff_t>(workers));
    std::latch start(1);
    std::vector<HRESULT> results(workers, E_PENDING);
    const std::vector<std::byte> reference_compressed =
        fast ? compressed_images.front() : std::vector<std::byte>{};
    std::vector<std::jthread> threads;
    threads.reserve(workers);
    for (std::size_t index = 0; index < workers; ++index) {
        threads.emplace_back([&, index] {
            ready.count_down();
            start.wait();
            results[index] = fast
                                 ? pv::DecodePngFast(compressed_images[index], *surfaces[index])
                                 : pv::DecodePngWuffs(compressed_images[index], *surfaces[index]);
        });
    }
    ready.wait();
    const auto begin = std::chrono::steady_clock::now();
    start.count_down();
    threads.clear();
    const auto elapsed = std::chrono::steady_clock::now() - begin;
    Check(std::all_of(results.begin(), results.end(),
                      [](const HRESULT result) { return SUCCEEDED(result); }),
          "Wuffs benchmark decode");
    bool pixel_match = true;
    if (fast) {
        pv::CpuSurface reference;
        reference.width = surfaces.front()->width;
        reference.height = surfaces.front()->height;
        reference.stride = reference.width * 4;
        const std::size_t reference_bytes =
            static_cast<std::size_t>(reference.stride) * reference.height;
        Check(reference.Allocate(reference_bytes), "allocate reference surface");
        Check(SUCCEEDED(pv::DecodePngWuffs(reference_compressed, reference)),
              "decode Wuffs reference");
        pixel_match = std::memcmp(reference.pixels, surfaces.front()->pixels,
                                  reference_bytes) == 0;
        Check(pixel_match, "fast decoder differs from Wuffs reference");
    }
    const double seconds = std::chrono::duration<double>(elapsed).count();
    const double load_seconds = std::chrono::duration<double>(load_elapsed).count();
    std::cout << "FileRead images=" << workers
              << " elapsed_ms=" << (load_seconds * 1000.0)
              << " mib_per_second=" << ((compressed_bytes / 1048576.0) / load_seconds)
              << '\n';
    std::cout << (fast ? "FastDecode" : "WuffsDecode")
              << " workers=" << workers << " images=" << workers
              << " elapsed_ms=" << (seconds * 1000.0)
              << " images_per_second=" << (workers / seconds)
              << " pixel_match=" << (pixel_match ? "true" : "false") << '\n';
    return 0;
}

int BenchmarkGraphics(const HINSTANCE instance) {
    constexpr wchar_t class_name[] = L"PhotoViewer.GraphicsBenchmark";
    WNDCLASSW window_class{};
    window_class.lpfnWndProc = DefWindowProcW;
    window_class.hInstance = instance;
    window_class.lpszClassName = class_name;
    Check(RegisterClassW(&window_class) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS,
          "register graphics benchmark window");
    HWND window = CreateWindowExW(0, class_name, L"graphics-benchmark",
                                  WS_OVERLAPPEDWINDOW, 0, 0, 1920, 1080,
                                  nullptr, nullptr, instance, nullptr);
    Check(window != nullptr, "create graphics benchmark window");

    pv::Graphics graphics;
    graphics.Initialize(window);
    auto surface = std::make_unique<pv::CpuSurface>();
    surface->width = 7680;
    surface->height = 4320;
    surface->stride = surface->width * 4;
    Check(surface->Allocate(static_cast<std::size_t>(surface->stride) * surface->height),
          "allocate graphics benchmark surface");
    std::memset(surface->pixels, 0x80, surface->ByteSize());
    Check(WaitForSingleObject(graphics.FrameWaitableObject(), 5000) == WAIT_OBJECT_0,
          "initial graphics benchmark frame credit");

    constexpr std::size_t frames = 30;
    const auto begin = std::chrono::steady_clock::now();
    for (std::size_t index = 0; index < frames; ++index) {
        pv::UploadTicket ticket = graphics.SubmitUpload(index, 1, std::move(surface));
        graphics.ArmFence(ticket.fence_value);
        Check(WaitForSingleObject(graphics.FenceEvent(), 5000) == WAIT_OBJECT_0,
              "graphics benchmark upload fence");
        pv::GpuImage image = graphics.FinishUpload(ticket);
        graphics.Draw(image);
        surface = std::move(ticket.source);
        if (index + 1 < frames) {
            Check(WaitForSingleObject(graphics.FrameWaitableObject(), 5000) == WAIT_OBJECT_0,
                  "graphics benchmark frame credit");
        }
    }
    const double seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - begin).count();
    std::cout << "Graphics8K frames=" << frames
              << " elapsed_ms=" << (seconds * 1000.0)
              << " frames_per_second=" << (frames / seconds) << '\n';
    DestroyWindow(window);
    return 0;
}

}  // namespace

int wmain(const int argc, wchar_t** const argv) {
    try {
        if (argc == 2 && std::wstring_view(argv[1]) == L"--graphics-benchmark") {
            return BenchmarkGraphics(GetModuleHandleW(nullptr));
        }
        if (argc == 4 &&
            (std::wstring_view(argv[1]) == L"--decode-benchmark" ||
             std::wstring_view(argv[1]) == L"--fast-decode-benchmark")) {
            const std::size_t workers = std::stoull(argv[3]);
            Check(workers > 0 && workers <= 64, "invalid benchmark worker count");
            return BenchmarkDecode(argv[2], workers,
                                   std::wstring_view(argv[1]) == L"--fast-decode-benchmark");
        }
        pv::CheckHr(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "CoInitializeEx");
        TestWic();
        TestGraphics(GetModuleHandleW(nullptr));
        CoUninitialize();
        std::cout << "PASS: WIC decode, D3D11 staging/fence, Direct2D draw, DXGI present\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
