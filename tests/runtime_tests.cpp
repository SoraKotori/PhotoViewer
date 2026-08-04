#include "common.h"
#include "decoder.h"
#include "graphics.h"
#include "spng_decoder.h"

#include <array>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <latch>

namespace {

void Check(const bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void TestSpng() {
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

    std::array<std::byte, 8> decoded{};
    pv::DecodeSurface surface{decoded.data(), decoded.size(), 4, 1, 1, 4};
    pv::CheckHr(pv::DecodePngSpng(png, surface), "Decode embedded PNG with libspng");
    Check(surface.pixels[3] == std::byte{0xFF}, "decoded alpha channel");

    const std::array<unsigned char, 68> fallback_raw{
        0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A,
        0x00,0x00,0x00,0x0D,0x49,0x48,0x44,0x52,
        0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,
        0x08,0x04,0x00,0x00,0x00,0xB5,0x1C,0x0C,0x02,
        0x00,0x00,0x00,0x0B,0x49,0x44,0x41,0x54,
        0x78,0xDA,0x63,0x64,0xF8,0x0F,0x00,0x01,
        0x05,0x01,0x01,0x27,0x18,0xE3,0x66,
        0x00,0x00,0x00,0x00,0x49,0x45,0x4E,0x44,
        0xAE,0x42,0x60,0x82};
    std::array<std::byte, fallback_raw.size()> fallback_png{};
    for (std::size_t index = 0; index < fallback_raw.size(); ++index) {
        fallback_png[index] = std::byte{fallback_raw[index]};
    }
    pv::CheckHr(pv::DecodePngSpng(fallback_png, surface),
                "Decode grayscale-alpha PNG with libspng/zlib-ng fallback");
}

void TestCancelledWorkReleasesInput() {
    pv::ResourceSlots slots(1, 1, 1, pv::MiB(1), pv::MiB(1));
    const pv::SlotId compressed_slot = slots.AcquireCompressed(4096, 0, 1);
    const pv::SlotId staging_slot = slots.AcquireStaging(4096, 0, 1);
    Check(compressed_slot != pv::kInvalidSlot &&
              staging_slot != pv::kInvalidSlot,
          "allocate cancellation test slots");

    auto token = std::make_shared<pv::WorkToken>();
    token->claim.store(pv::WorkClaim::Cancelled, std::memory_order_release);
    pv::WorkQueue work_queue(1);
    pv::CompletionQueue completion_queue;
    {
        pv::DecoderPool pool(1, work_queue, completion_queue, slots, nullptr);
        pv::DecodeWork work{0, 1, token, compressed_slot, staging_slot};
        Check(work_queue.TryPush(work), "queue pre-claim cancellation test work");

        std::vector<pv::DecodeResult> results;
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(5);
        while (results.empty() && std::chrono::steady_clock::now() < deadline) {
            results = completion_queue.Drain();
            if (results.empty()) Sleep(1);
        }
        Check(results.size() == 1 && results.front().cancelled,
              "worker must report work cancelled before claim");

        std::vector<pv::ReleasedInput> released =
            completion_queue.DrainReleasedInputs();
        Check(released.size() == 1 &&
                  released.front().compressed_slot == compressed_slot,
              "cancelled work must release its compressed input exactly once");
    }

    slots.ReleaseCompressed(compressed_slot);
    slots.ReleaseStaging(staging_slot);
    Check(slots.FreeCompressedCount() == 1 &&
              slots.FreeStagingCount() == 1,
          "cancelled work slots must return to their free indexes");
}

void TestManagedStagingUpload() {
    pv::ComPtr<ID3D11Device> device;
    pv::ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL feature_level{};
    constexpr std::array feature_levels{D3D_FEATURE_LEVEL_11_1,
                                        D3D_FEATURE_LEVEL_11_0};
    pv::CheckHr(D3D11CreateDevice(
                    nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                    feature_levels.data(), static_cast<UINT>(feature_levels.size()),
                    D3D11_SDK_VERSION, &device, &feature_level, &context),
                "Create D3D11 device for managed staging test");

    constexpr UINT width = 16;
    constexpr UINT height = 16;
    constexpr UINT stride = width * 4;
    std::array<std::byte, stride * height> source{};
    for (std::size_t offset = 0; offset < source.size(); offset += 4) {
        source[offset + 0] = std::byte{0x12};
        source[offset + 1] = std::byte{0x34};
        source[offset + 2] = std::byte{0x56};
        source[offset + 3] = std::byte{0xFF};
    }

    D3D11_TEXTURE2D_DESC description{};
    description.Width = width;
    description.Height = height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    pv::ComPtr<ID3D11Texture2D> texture;
    pv::CheckHr(device->CreateTexture2D(&description, nullptr, &texture),
                "Create destination texture for managed staging test");

    description.Usage = D3D11_USAGE_STAGING;
    description.BindFlags = 0;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    pv::ComPtr<ID3D11Texture2D> upload;
    pv::CheckHr(device->CreateTexture2D(&description, nullptr, &upload),
                "Create managed upload staging texture");
    D3D11_MAPPED_SUBRESOURCE upload_mapping{};
    pv::CheckHr(context->Map(upload.Get(), 0, D3D11_MAP_WRITE, 0,
                            &upload_mapping),
                "Map managed upload staging texture");
    for (UINT row = 0; row < height; ++row) {
        std::memcpy(static_cast<std::byte*>(upload_mapping.pData) +
                        static_cast<std::size_t>(row) * upload_mapping.RowPitch,
                    source.data() + static_cast<std::size_t>(row) * stride,
                    stride);
    }
    context->Unmap(upload.Get(), 0);

    std::memset(source.data(), 0xA5, source.size());
    context->CopyResource(texture.Get(), upload.Get());

    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    pv::ComPtr<ID3D11Texture2D> readback;
    pv::CheckHr(device->CreateTexture2D(&description, nullptr, &readback),
                "Create readback staging texture for managed staging test");
    context->CopyResource(readback.Get(), texture.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    pv::CheckHr(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped),
                "Read texture uploaded through managed staging");
    const auto* const pixel = static_cast<const std::byte*>(mapped.pData);
    const bool retained = pixel[0] == std::byte{0x12} &&
                          pixel[1] == std::byte{0x34} &&
                          pixel[2] == std::byte{0x56} &&
                          pixel[3] == std::byte{0xFF};
    context->Unmap(readback.Get(), 0);
    Check(retained, "managed staging upload must preserve source pixels");
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
        pv::DecodeStaging staging;
        graphics.MapDecodeStaging(staging, 64, 64, 64 * 64 * 4);
        for (UINT row = 0; row < staging.surface.height; ++row) {
            std::byte* const pixels = staging.surface.pixels +
                static_cast<std::size_t>(row) * staging.surface.stride;
            for (UINT column = 0; column < staging.surface.width; ++column) {
                pixels[column * 4 + 0] = std::byte{0x20};
                pixels[column * 4 + 1] = std::byte{0x80};
                pixels[column * 4 + 2] = std::byte{0xE0};
                pixels[column * 4 + 3] = std::byte{0xFF};
            }
        }
        graphics.UnmapDecodeStaging(staging);
        pv::GpuImage image;
        pv::UploadTicket ticket = graphics.SubmitUpload(0, 1, 0, staging, image);
        graphics.ArmFence(ticket.fence_value);
        Check(WaitForSingleObject(graphics.FenceEvent(), 5000) == WAIT_OBJECT_0,
              "D3D11 fence completion");
        graphics.FinishUpload(image);
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

int BenchmarkDecode(const std::filesystem::path& path, const std::size_t workers) {
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

    std::vector<std::vector<std::byte>> pixel_storage;
    std::vector<pv::DecodeSurface> surfaces;
    pixel_storage.reserve(workers);
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
        pixel_storage.emplace_back(decoded_bytes + height);
        pv::DecodeSurface surface{pixel_storage.back().data(),
                                  pixel_storage.back().size(), decoded_bytes,
                                  width, height, width * 4};
        compressed_images.push_back(std::move(compressed));
        surfaces.push_back(std::move(surface));
    }
    const auto load_elapsed = std::chrono::steady_clock::now() - load_begin;

    std::latch ready(static_cast<std::ptrdiff_t>(workers));
    std::latch start(1);
    std::vector<HRESULT> results(workers, E_PENDING);
    std::vector<double> worker_milliseconds(workers, 0.0);
    std::vector<pv::PngDecodeTimings> decode_timings(workers);
    std::vector<std::jthread> threads;
    threads.reserve(workers);
    for (std::size_t index = 0; index < workers; ++index) {
        threads.emplace_back([&, index] {
            ready.count_down();
            start.wait();
            const auto worker_begin = std::chrono::steady_clock::now();
            results[index] = pv::DecodePngSpng(compressed_images[index],
                                               surfaces[index], nullptr, nullptr,
                                               &decode_timings[index]);
            worker_milliseconds[index] = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - worker_begin).count();
        });
    }
    ready.wait();
    const auto begin = std::chrono::steady_clock::now();
    start.count_down();
    threads.clear();
    const auto elapsed = std::chrono::steady_clock::now() - begin;
    Check(std::all_of(results.begin(), results.end(),
                      [](const HRESULT result) { return SUCCEEDED(result); }),
          "PNG benchmark decode");
    std::uint64_t sampled_digest = 1469598103934665603ULL;
    for (const auto& surface : surfaces) {
        for (std::size_t offset = 0; offset < surface.ByteSize(); offset += 4096) {
            sampled_digest ^= static_cast<std::uint8_t>(surface.pixels[offset]);
            sampled_digest *= 1099511628211ULL;
        }
        sampled_digest ^= static_cast<std::uint8_t>(
            surface.pixels[surface.ByteSize() - 1]);
        sampled_digest *= 1099511628211ULL;
    }
    std::sort(worker_milliseconds.begin(), worker_milliseconds.end());
    const auto percentile = [&](const double value) {
        const std::size_t rank = static_cast<std::size_t>(
            std::ceil(value * static_cast<double>(worker_milliseconds.size())));
        return worker_milliseconds[std::min(worker_milliseconds.size() - 1,
                                            std::max<std::size_t>(1, rank) - 1)];
    };
    const double seconds = std::chrono::duration<double>(elapsed).count();
    const double load_seconds = std::chrono::duration<double>(load_elapsed).count();
    pv::PngDecodeTimings total_timings;
    for (const auto& timing : decode_timings) {
        total_timings.header_nanoseconds += timing.header_nanoseconds;
        total_timings.chunk_scan_nanoseconds += timing.chunk_scan_nanoseconds;
        total_timings.idat_compaction_nanoseconds += timing.idat_compaction_nanoseconds;
        total_timings.deflate_nanoseconds += timing.deflate_nanoseconds;
        total_timings.unfilter_nanoseconds += timing.unfilter_nanoseconds;
        for (std::size_t filter = 0; filter < total_timings.filter_rows.size(); ++filter) {
            total_timings.filter_rows[filter] += timing.filter_rows[filter];
        }
    }
    const auto average_ms = [workers](const std::uint64_t nanoseconds) {
        return static_cast<double>(nanoseconds) / static_cast<double>(workers) / 1.0e6;
    };
    std::cout << "FileRead images=" << workers
              << " elapsed_ms=" << (load_seconds * 1000.0)
              << " mib_per_second=" << ((compressed_bytes / 1048576.0) / load_seconds)
              << '\n';
    std::cout << "LibdeflateDecode"
              << " workers=" << workers << " images=" << workers
              << " batch_elapsed_ms=" << (seconds * 1000.0)
              << " images_per_second=" << (workers / seconds)
              << " p50_worker_ms=" << percentile(0.50)
              << " p95_worker_ms=" << percentile(0.95)
              << " max_worker_ms=" << worker_milliseconds.back() << '\n';
    std::cout << "DecodeStages"
              << " header_ms=" << average_ms(total_timings.header_nanoseconds)
              << " chunk_scan_ms=" << average_ms(total_timings.chunk_scan_nanoseconds)
              << " idat_compaction_ms="
              << average_ms(total_timings.idat_compaction_nanoseconds)
              << " deflate_ms=" << average_ms(total_timings.deflate_nanoseconds)
              << " unfilter_ms=" << average_ms(total_timings.unfilter_nanoseconds)
              << " filter_none=" << total_timings.filter_rows[0]
              << " filter_sub=" << total_timings.filter_rows[1]
              << " filter_up=" << total_timings.filter_rows[2]
              << " filter_average=" << total_timings.filter_rows[3]
              << " filter_paeth=" << total_timings.filter_rows[4]
              << " sampled_digest=" << sampled_digest << '\n';
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
    pv::DecodeStaging staging;
    constexpr UINT width = 7680;
    constexpr UINT height = 4320;
    constexpr std::size_t decoded_bytes =
        static_cast<std::size_t>(width) * height * 4;
    graphics.MapDecodeStaging(staging, width, height, decoded_bytes);
    for (UINT row = 0; row < height; ++row) {
        std::memset(staging.surface.pixels +
                        static_cast<std::size_t>(row) * staging.surface.stride,
                    0x80, static_cast<std::size_t>(width) * 4);
    }
    graphics.UnmapDecodeStaging(staging);
    Check(WaitForSingleObject(graphics.FrameWaitableObject(), 5000) == WAIT_OBJECT_0,
          "initial graphics benchmark frame credit");

    constexpr std::size_t frames = 30;
    const auto begin = std::chrono::steady_clock::now();
    for (std::size_t index = 0; index < frames; ++index) {
        pv::GpuImage image;
        pv::UploadTicket ticket = graphics.SubmitUpload(index, 1, 0, staging, image);
        graphics.ArmFence(ticket.fence_value);
        Check(WaitForSingleObject(graphics.FenceEvent(), 5000) == WAIT_OBJECT_0,
              "graphics benchmark upload fence");
        graphics.FinishUpload(image);
        graphics.Draw(image);
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
        if (argc == 4 && std::wstring_view(argv[1]) == L"--decode-benchmark") {
            const std::size_t workers = std::stoull(argv[3]);
            Check(workers > 0 && workers <= 64, "invalid benchmark worker count");
            return BenchmarkDecode(argv[2], workers);
        }
        TestSpng();
        TestCancelledWorkReleasesInput();
        TestManagedStagingUpload();
        TestGraphics(GetModuleHandleW(nullptr));
        std::cout << "PASS: pre-claim cancellation slot return, libspng/libdeflate decode with zlib-ng fallback, managed D3D11 staging upload/fence, Direct2D draw, DXGI present\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
