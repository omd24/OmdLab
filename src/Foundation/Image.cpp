#include "Image.h"

#include "Log.h"

#ifdef OMD_WINDOWS

    #define WIN32_LEAN_AND_MEAN
    #include <Windows.h>

    #include <mutex>
    #include <vector>
    #include <wincodec.h>
    #include <wrl/client.h>

namespace
{
    using Microsoft::WRL::ComPtr;
    using Foundation::Log::Severity;

    // WIC needs COM initialized on the calling thread. This is a dev-tooling leaf helper, so:
    // initialize once, tolerate "already initialized" (S_FALSE) and "initialized in another
    // apartment mode elsewhere" (RPC_E_CHANGED_MODE) - WIC works regardless - and never
    // CoUninitialize (process lifetime).
    void EnsureComInitialized()
    {
        static std::once_flag once;
        std::call_once(once, [] { CoInitializeEx(nullptr, COINIT_MULTITHREADED); });
    }

    std::wstring WidenUtf8(const std::string& utf8)
    {
        const int wide = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
        if (wide <= 0)
        {
            return {};
        }
        std::wstring out(static_cast<size_t>(wide - 1), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, out.data(), wide);
        return out;
    }
}

namespace Foundation
{
    bool WritePng(const std::string& path, unsigned width, unsigned height, const std::uint8_t* rgba, unsigned rowPitchBytes)
    {
        if (rgba == nullptr || width == 0 || height == 0 || rowPitchBytes < width * 4u)
        {
            Log::Write(Severity::Error, "Image", "WritePng: invalid args for '%s' (%ux%u pitch %u)", path.c_str(), width, height, rowPitchBytes);
            return false;
        }

        EnsureComInitialized();

        ComPtr<IWICImagingFactory> factory;
        HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
        if (FAILED(hr))
        {
            Log::Write(Severity::Error, "Image", "WritePng: WIC factory failed (0x%08lX)", static_cast<unsigned long>(hr));
            return false;
        }

        const std::wstring widePath = WidenUtf8(path);

        ComPtr<IWICStream> stream;
        hr = factory->CreateStream(&stream);
        if (SUCCEEDED(hr))
        {
            hr = stream->InitializeFromFilename(widePath.c_str(), GENERIC_WRITE);
        }
        if (FAILED(hr))
        {
            Log::Write(Severity::Error, "Image", "WritePng: cannot open '%s' (0x%08lX)", path.c_str(), static_cast<unsigned long>(hr));
            return false;
        }

        ComPtr<IWICBitmapEncoder> encoder;
        hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
        if (SUCCEEDED(hr))
        {
            hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
        }

        ComPtr<IWICBitmapFrameEncode> frame;
        ComPtr<IPropertyBag2> frameProps;
        if (SUCCEEDED(hr))
        {
            hr = encoder->CreateNewFrame(&frame, &frameProps);
        }
        if (SUCCEEDED(hr))
        {
            hr = frame->Initialize(frameProps.Get());
        }
        if (SUCCEEDED(hr))
        {
            hr = frame->SetSize(width, height);
        }

        // WIC's PNG encoder supports 32bpp *BGRA*, not RGBA - asking for RGBA makes it silently
        // substitute its own choice. So target BGRA and swizzle R<->B into a tight scratch
        // buffer (a full-image copy - trivial for a dev screenshot).
        WICPixelFormatGUID pixelFormat = GUID_WICPixelFormat32bppBGRA;
        if (SUCCEEDED(hr))
        {
            hr = frame->SetPixelFormat(&pixelFormat);
        }
        if (SUCCEEDED(hr) && pixelFormat != GUID_WICPixelFormat32bppBGRA)
        {
            Log::Write(Severity::Error, "Image", "WritePng: WIC would not accept 32bppBGRA for '%s'", path.c_str());
            return false;
        }

        std::vector<std::uint8_t> bgra;
        if (SUCCEEDED(hr))
        {
            const size_t tightPitch = static_cast<size_t>(width) * 4;
            bgra.resize(tightPitch * height);
            for (unsigned y = 0; y < height; ++y)
            {
                const std::uint8_t* srcRow = rgba + static_cast<size_t>(y) * rowPitchBytes;
                std::uint8_t* dstRow = bgra.data() + y * tightPitch;
                for (unsigned x = 0; x < width; ++x)
                {
                    dstRow[x * 4 + 0] = srcRow[x * 4 + 2]; // B
                    dstRow[x * 4 + 1] = srcRow[x * 4 + 1]; // G
                    dstRow[x * 4 + 2] = srcRow[x * 4 + 0]; // R
                    dstRow[x * 4 + 3] = srcRow[x * 4 + 3]; // A
                }
            }
            const UINT dstStride = width * 4u;
            hr = frame->WritePixels(height, dstStride, dstStride * height, bgra.data());
        }
        if (SUCCEEDED(hr))
        {
            hr = frame->Commit();
        }
        if (SUCCEEDED(hr))
        {
            hr = encoder->Commit();
        }
        if (FAILED(hr))
        {
            Log::Write(Severity::Error, "Image", "WritePng: encode '%s' failed (0x%08lX)", path.c_str(), static_cast<unsigned long>(hr));
            return false;
        }

        return true;
    }
}

#else // !OMD_WINDOWS

namespace Foundation
{
    bool WritePng(const std::string& path, unsigned, unsigned, const std::uint8_t*, unsigned)
    {
        Log::Write(Foundation::Log::Severity::Error, "Image", "WritePng: not implemented on this platform ('%s')", path.c_str());
        return false;
    }
}

#endif
