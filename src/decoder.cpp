#include "decoder.h"

#include "common.h"
#include "fast_png.h"
#include "wuffs_png.h"

namespace pv {

HRESULT DecodePngMemory(IWICImagingFactory* const factory,
                        const std::span<const std::byte> compressed,
                        CpuSurface& surface) noexcept {
    const HRESULT fast_result = DecodePngWuffs(compressed, surface);
    if (SUCCEEDED(fast_result)) return fast_result;
    if (!factory || compressed.empty() ||
        compressed.size() > std::numeric_limits<DWORD>::max()) {
        return E_INVALIDARG;
    }
    ComPtr<IWICStream> stream;
    HRESULT hr = factory->CreateStream(&stream);
    if (SUCCEEDED(hr)) {
        hr = stream->InitializeFromMemory(
            reinterpret_cast<BYTE*>(const_cast<std::byte*>(compressed.data())),
            static_cast<DWORD>(compressed.size()));
    }
    ComPtr<IWICBitmapDecoder> decoder;
    if (SUCCEEDED(hr)) {
        hr = factory->CreateDecoderFromStream(stream.Get(), nullptr,
                                              WICDecodeMetadataCacheOnLoad, &decoder);
    }
    ComPtr<IWICBitmapFrameDecode> frame;
    if (SUCCEEDED(hr)) hr = decoder->GetFrame(0, &frame);
    UINT width = 0;
    UINT height = 0;
    if (SUCCEEDED(hr)) hr = frame->GetSize(&width, &height);
    if (SUCCEEDED(hr) && (width != surface.width || height != surface.height)) {
        hr = E_INVALIDARG;
    }
    if (SUCCEEDED(hr) &&
        (!surface.pixels || surface.ByteSize() > std::numeric_limits<UINT>::max())) {
        hr = E_OUTOFMEMORY;
    }

    WICPixelFormatGUID native_format{};
    if (SUCCEEDED(hr)) hr = frame->GetPixelFormat(&native_format);
    if (SUCCEEDED(hr) && IsEqualGUID(native_format, GUID_WICPixelFormat32bppRGBA)) {
        return frame->CopyPixels(nullptr, surface.stride,
                                 static_cast<UINT>(surface.ByteSize()),
                                 reinterpret_cast<BYTE*>(surface.pixels));
    }

    ComPtr<IWICBitmapSourceTransform> transform;
    if (SUCCEEDED(hr) && SUCCEEDED(frame.As(&transform))) {
        UINT transformed_width = width;
        UINT transformed_height = height;
        WICPixelFormatGUID transformed_format = GUID_WICPixelFormat32bppRGBA;
        BOOL supported = FALSE;
        HRESULT transform_hr = transform->DoesSupportTransform(
            WICBitmapTransformRotate0, &supported);
        if (SUCCEEDED(transform_hr) && supported) {
            transform_hr = transform->GetClosestSize(&transformed_width,
                                                      &transformed_height);
        }
        if (SUCCEEDED(transform_hr)) {
            transform_hr = transform->GetClosestPixelFormat(&transformed_format);
        }
        if (SUCCEEDED(transform_hr) && transformed_width == width &&
            transformed_height == height &&
            IsEqualGUID(transformed_format, GUID_WICPixelFormat32bppRGBA)) {
            transform_hr = transform->CopyPixels(
                nullptr, width, height, &transformed_format, WICBitmapTransformRotate0,
                surface.stride, static_cast<UINT>(surface.ByteSize()),
                reinterpret_cast<BYTE*>(surface.pixels));
            if (SUCCEEDED(transform_hr)) return transform_hr;
        }
    }

    ComPtr<IWICFormatConverter> converter;
    if (SUCCEEDED(hr)) hr = factory->CreateFormatConverter(&converter);
    if (SUCCEEDED(hr)) {
        hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
                                   WICBitmapDitherTypeNone, nullptr, 0.0,
                                   WICBitmapPaletteTypeCustom);
    }
    if (SUCCEEDED(hr)) {
        hr = converter->CopyPixels(nullptr, surface.stride,
                                   static_cast<UINT>(surface.ByteSize()),
                                   reinterpret_cast<BYTE*>(surface.pixels));
    }
    return hr;
}

DecoderPool::DecoderPool(const std::size_t worker_count, WorkQueue& work_queue,
                         CompletionQueue& completion_queue, const HWND event_window)
    : work_queue_(work_queue), completion_queue_(completion_queue), event_window_(event_window) {
    workers_.reserve(worker_count);
    for (std::size_t index = 0; index < worker_count; ++index) {
        workers_.emplace_back([this](const std::stop_token stop) { WorkerMain(stop); });
    }
}

DecoderPool::~DecoderPool() {
    for (auto& worker : workers_) worker.request_stop();
    work_queue_.Stop();
    workers_.clear();
}

void DecoderPool::WorkerMain(const std::stop_token stop) {
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) return;
    const bool uninitialize = SUCCEEDED(initialized);

    ComPtr<IWICImagingFactory> factory;
    const HRESULT factory_result = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                                     CLSCTX_INPROC_SERVER,
                                                     IID_PPV_ARGS(&factory));
    if (SUCCEEDED(factory_result)) {
        DecodeWork work;
        while (work_queue_.Pop(work, stop)) {
            DecodeResult result = Decode(factory.Get(), std::move(work));
            completion_queue_.Push(std::move(result));
            PostMessageW(event_window_, kMessageWorkerComplete, 0, 0);
            work = {};
        }
    }
    if (uninitialize) CoUninitialize();
}

DecodeResult DecoderPool::Decode(IWICImagingFactory* const factory, DecodeWork work) {
    DecodeResult result;
    result.index = work.index;
    result.generation = work.generation;
    result.surface = std::move(work.surface);

    WorkClaim expected = WorkClaim::Queued;
    if (!work.token || !work.token->claim.compare_exchange_strong(
                           expected, WorkClaim::Claimed, std::memory_order_acq_rel)) {
        result.cancelled = true;
        return result;
    }
    if (!work.compressed || !work.compressed->data || work.compressed->size == 0 ||
        !result.surface ||
        !result.surface->pixels) {
        result.error = E_INVALIDARG;
        return result;
    }
    struct CallbackContext {
        DecoderPool* pool;
        DecodeWork* work;
    } callback_context{this, &work};
    const auto input_consumed = [](void* const raw) noexcept {
        auto* const context = static_cast<CallbackContext*>(raw);
        context->pool->ReleaseInput(*context->work);
    };
    HRESULT hr = DecodePngFast(
        std::span(work.compressed->data, work.compressed->size), *result.surface,
        input_consumed, &callback_context);
    if (hr == WINCODEC_ERR_COMPONENTNOTFOUND) {
        hr = DecodePngMemory(factory,
                             std::span<const std::byte>(work.compressed->data,
                                                        work.compressed->size),
                             *result.surface);
    }
    ReleaseInput(work);
    result.error = hr;
    result.cancelled = work.token->claim.load(std::memory_order_acquire) == WorkClaim::Cancelled;
    result.success = SUCCEEDED(hr) && !result.cancelled;
    return result;
}

void DecoderPool::ReleaseInput(DecodeWork& work) noexcept {
    if (!work.compressed) return;
    completion_queue_.PushReleasedInput(ReleasedInput{std::move(work.compressed)});
    PostMessageW(event_window_, kMessageWorkerComplete, 0, 0);
}

}  // namespace pv
