#include "ImageExport.h"

#include "FileIO.h"
#include "Trace.h"

#include <wincodec.h>
#include <windows.h>
#include <wrl/client.h>

#include <vector>

#pragma comment(lib, "windowscodecs.lib")

namespace pdf {

using Microsoft::WRL::ComPtr;

namespace {
Error Fail(const char* what, HRESULT hr) {
    TraceF("[PDFTool.Export] %s failed, hr=0x%08lX\n", what, static_cast<unsigned long>(hr));
    return Error{L"导出图片失败", static_cast<unsigned long>(hr)};
}
}  // namespace

Result<std::vector<std::uint8_t>> EncodePng(const Bitmap& bitmap) {
    if (bitmap.width <= 0 || bitmap.height <= 0 ||
        bitmap.pixels.size() != static_cast<std::size_t>(bitmap.width) * bitmap.height * 4)
        return Error{L"位图无效"};

    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return Fail("CoCreateInstance(WIC)", hr);

    // Encode into a growable memory stream, then write the bytes atomically.
    ComPtr<IStream> stream;
    hr = CreateStreamOnHGlobal(nullptr, TRUE, &stream);
    if (FAILED(hr)) return Fail("CreateStreamOnHGlobal", hr);

    ComPtr<IWICBitmapEncoder> encoder;
    hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (FAILED(hr)) return Fail("CreateEncoder", hr);
    hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (FAILED(hr)) return Fail("Encoder::Initialize", hr);

    ComPtr<IWICBitmapFrameEncode> frame;
    hr = encoder->CreateNewFrame(&frame, nullptr);
    if (FAILED(hr)) return Fail("CreateNewFrame", hr);
    hr = frame->Initialize(nullptr);
    if (FAILED(hr)) return Fail("Frame::Initialize", hr);
    hr = frame->SetSize(static_cast<UINT>(bitmap.width), static_cast<UINT>(bitmap.height));
    if (FAILED(hr)) return Fail("SetSize", hr);

    // Pages render onto opaque white, so plain BGRA (alpha 255) is exact.
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    hr = frame->SetPixelFormat(&format);
    if (FAILED(hr)) return Fail("SetPixelFormat", hr);
    if (format != GUID_WICPixelFormat32bppBGRA) return Error{L"PNG 编码器不支持 BGRA"};

    hr = frame->WritePixels(static_cast<UINT>(bitmap.height), static_cast<UINT>(bitmap.Stride()),
                            static_cast<UINT>(bitmap.pixels.size()),
                            const_cast<BYTE*>(bitmap.pixels.data()));
    if (FAILED(hr)) return Fail("WritePixels", hr);
    if (FAILED(hr = frame->Commit())) return Fail("Frame::Commit", hr);
    if (FAILED(hr = encoder->Commit())) return Fail("Encoder::Commit", hr);

    HGLOBAL mem = nullptr;
    if (FAILED(hr = GetHGlobalFromStream(stream.Get(), &mem))) return Fail("GetHGlobal", hr);
    STATSTG st{};
    if (FAILED(hr = stream->Stat(&st, STATFLAG_NONAME))) return Fail("Stat", hr);
    const auto size = static_cast<std::size_t>(st.cbSize.QuadPart);
    const void* src = GlobalLock(mem);
    if (!src) return Error{L"导出图片失败"};
    try {
        std::vector<std::uint8_t> bytes(static_cast<const std::uint8_t*>(src),
                                        static_cast<const std::uint8_t*>(src) + size);
        GlobalUnlock(mem);
        return bytes;
    } catch (const std::bad_alloc&) {
        GlobalUnlock(mem);
        return Error{L"内存不足"};
    }
}

Status SavePng(const Bitmap& bitmap, const std::wstring& path) {
    auto bytes = EncodePng(bitmap);
    if (!bytes) return bytes.error();
    return WriteFileAtomic(path, bytes.value());
}

}  // namespace pdf
