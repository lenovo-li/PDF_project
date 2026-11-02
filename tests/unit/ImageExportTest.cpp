// ImageExport: a rendered page written as PNG and decoded back with WIC must
// have the same size and pixels. Runs on the engine thread, which initializes
// COM for WIC.

#include "../framework/Test.h"
#include "../framework/TestEngine.h"

#include <PDFCore/Document.h>
#include <PDFCore/ImageExport.h>

#include <wincodec.h>
#include <windows.h>
#include <wrl/client.h>

using namespace pdf;
using Microsoft::WRL::ComPtr;

namespace {
std::wstring TempPath(const wchar_t* name) {
    wchar_t dir[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    return std::wstring(dir) + name;
}
}  // namespace

TEST(ImageExport, PngRoundTripKeepsSizeAndPixels) {
    const std::wstring path = TempPath(L"PDFTool_export_test.png");
    pdftest::OnEngine([&] {
        // A known bitmap: left half red, right half white.
        Bitmap b;
        b.width = 40;
        b.height = 20;
        b.pixels.resize(40 * 20 * 4, 0xFF);
        for (int y = 0; y < 20; ++y)
            for (int x = 0; x < 20; ++x) {
                std::uint8_t* px = &b.pixels[(static_cast<std::size_t>(y) * 40 + x) * 4];
                px[0] = 0; px[1] = 0;  // B, G = 0 -> red
            }
        EXPECT_TRUE(SavePng(b, path).ok());

        ComPtr<IWICImagingFactory> f;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&f)))) {
            EXPECT_TRUE(false);
            return;
        }
        ComPtr<IWICBitmapDecoder> dec;
        EXPECT_TRUE(SUCCEEDED(f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                          WICDecodeMetadataCacheOnDemand, &dec)));
        if (!dec) return;
        ComPtr<IWICBitmapFrameDecode> frame;
        dec->GetFrame(0, &frame);
        if (!frame) { EXPECT_TRUE(false); return; }
        UINT w = 0, h = 0;
        frame->GetSize(&w, &h);
        EXPECT_EQ(w, 40u);
        EXPECT_EQ(h, 20u);
        ComPtr<IWICFormatConverter> conv;
        f->CreateFormatConverter(&conv);
        conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone,
                         nullptr, 0, WICBitmapPaletteTypeCustom);
        std::vector<BYTE> back(40 * 20 * 4);
        EXPECT_TRUE(SUCCEEDED(conv->CopyPixels(nullptr, 40 * 4, static_cast<UINT>(back.size()),
                                               back.data())));
        EXPECT_EQ(back[(10 * 40 + 5) * 4 + 2], 0xFF);   // red at (5,10)
        EXPECT_EQ(back[(10 * 40 + 5) * 4 + 1], 0x00);
        EXPECT_EQ(back[(10 * 40 + 30) * 4 + 1], 0xFF);  // white at (30,10)
    });
    DeleteFileW(path.c_str());
}

TEST(ImageExport, RejectsInvalidBitmap) {
    pdftest::OnEngine([] {
        const std::wstring temp = TempPath(L"PDFTool_invalidbmp.png");
        Bitmap empty;
        EXPECT_FALSE(SavePng(empty, temp).ok());
        Bitmap wrong;
        wrong.width = 10;
        wrong.height = 10;
        wrong.pixels.resize(5);  // too small for 10x10
        EXPECT_FALSE(SavePng(wrong, temp).ok());
        DeleteFileW(temp.c_str());
    });
}
