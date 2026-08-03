#include "wuffs_png.h"

#include <new>

#pragma warning(push, 0)
#define WUFFS_IMPLEMENTATION
#define WUFFS_CONFIG__STATIC_FUNCTIONS
#define WUFFS_CONFIG__MODULES
#define WUFFS_CONFIG__MODULE__BASE
#define WUFFS_CONFIG__MODULE__ADLER32
#define WUFFS_CONFIG__MODULE__CRC32
#define WUFFS_CONFIG__MODULE__DEFLATE
#define WUFFS_CONFIG__MODULE__PNG
#define WUFFS_CONFIG__MODULE__ZLIB
#define WUFFS_CONFIG__DST_PIXEL_FORMAT__ENABLE_ALLOWLIST
#define WUFFS_CONFIG__DST_PIXEL_FORMAT__ALLOW_RGBA_NONPREMUL
#include "../third_party/wuffs/wuffs-v0.3.c"
#pragma warning(pop)

namespace pv {
namespace {

bool Failed(const wuffs_base__status status) noexcept {
    return status.repr != nullptr;
}

}  // namespace

HRESULT DecodePngWuffs(const std::span<const std::byte> compressed,
                       CpuSurface& surface) noexcept {
    if (compressed.empty() || !surface.pixels || surface.ByteSize() == 0) {
        return E_INVALIDARG;
    }
    try {
        wuffs_png__decoder decoder{};
        if (Failed(wuffs_png__decoder__initialize(
                &decoder, sizeof(decoder), WUFFS_VERSION,
                WUFFS_INITIALIZE__DEFAULT_OPTIONS))) {
            return WINCODEC_ERR_BADIMAGE;
        }
        wuffs_png__decoder__set_quirk_enabled(
            &decoder, WUFFS_BASE__QUIRK_IGNORE_CHECKSUM, true);

        wuffs_base__io_buffer source = wuffs_base__ptr_u8__reader(
            reinterpret_cast<uint8_t*>(const_cast<std::byte*>(compressed.data())),
            compressed.size(), true);
        wuffs_base__image_config image_config{};
        if (Failed(wuffs_png__decoder__decode_image_config(
                &decoder, &image_config, &source))) {
            return WINCODEC_ERR_BADIMAGE;
        }

        const uint32_t width = wuffs_base__pixel_config__width(&image_config.pixcfg);
        const uint32_t height = wuffs_base__pixel_config__height(&image_config.pixcfg);
        if (width != surface.width || height != surface.height) return E_INVALIDARG;
        wuffs_base__pixel_config__set(
            &image_config.pixcfg, WUFFS_BASE__PIXEL_FORMAT__RGBA_NONPREMUL,
            WUFFS_BASE__PIXEL_SUBSAMPLING__NONE, width, height);

        wuffs_base__pixel_buffer pixel_buffer{};
        if (surface.stride < (width * 4U) ||
            Failed(wuffs_base__pixel_buffer__set_interleaved(
                &pixel_buffer, &image_config.pixcfg,
                wuffs_base__make_table_u8(
                    reinterpret_cast<uint8_t*>(surface.pixels), surface.stride,
                    height, surface.stride),
                wuffs_base__empty_slice_u8()))) {
            return E_INVALIDARG;
        }

        const wuffs_base__range_ii_u64 workbuf_range =
            wuffs_png__decoder__workbuf_len(&decoder);
        if (workbuf_range.max_incl > std::numeric_limits<std::size_t>::max()) {
            return E_OUTOFMEMORY;
        }
        std::vector<uint8_t> workbuf(static_cast<std::size_t>(workbuf_range.max_incl));

        wuffs_base__frame_config frame_config{};
        if (Failed(wuffs_png__decoder__decode_frame_config(
                &decoder, &frame_config, &source))) {
            return WINCODEC_ERR_BADIMAGE;
        }
        const wuffs_base__status decoded = wuffs_png__decoder__decode_frame(
            &decoder, &pixel_buffer, &source, WUFFS_BASE__PIXEL_BLEND__SRC,
            wuffs_base__make_slice_u8(workbuf.data(), workbuf.size()), nullptr);
        return Failed(decoded) ? WINCODEC_ERR_BADIMAGE : S_OK;
    } catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    } catch (...) {
        return E_FAIL;
    }
}

}  // namespace pv
