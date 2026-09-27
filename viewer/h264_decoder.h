// H.264 Annex-B -> NV12 with the Media Foundation decoder that ships with Windows
// (low-latency mode: one picture out per access unit in). Used for AXRI v8/v9 frames.
#pragma once

#include <windows.h>
#include <codecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wmcodecdsp.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>

class H264Decoder {
public:
    // One decoded picture. width/height are the decoder's (macroblock-aligned) size;
    // the chroma plane is interleaved UV at half resolution, same pitch.
    struct Picture {
        const uint8_t* y = nullptr;
        const uint8_t* uv = nullptr;
        UINT pitch = 0, width = 0, height = 0;
    };
    using Callback = std::function<void(const Picture&)>;

    // Call on the decoding thread (initializes COM there). A new key frame always starts with SPS/PPS.
    bool decode(const uint8_t* data, size_t size, int64_t time, bool key, UINT width, UINT height, const Callback& onPicture)
    {
        if (!mft_ && (!key || !create(width, height))) return false;  // A (new) decoder starts at a key frame.
        Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
        Microsoft::WRL::ComPtr<IMFSample> sample;
        BYTE* bytes = nullptr;
        if (FAILED(MFCreateMemoryBuffer(static_cast<DWORD>(size), &buffer)) || FAILED(buffer->Lock(&bytes, nullptr, nullptr))) return false;
        std::memcpy(bytes, data, size);
        buffer->Unlock();
        buffer->SetCurrentLength(static_cast<DWORD>(size));
        if (FAILED(MFCreateSample(&sample)) || FAILED(sample->AddBuffer(buffer.Get()))) return false;
        sample->SetSampleTime(time);
        HRESULT hr = mft_->ProcessInput(0, sample.Get(), 0);
        if (hr == MF_E_NOTACCEPTING) {
            if (!drain(onPicture)) return reset();
            hr = mft_->ProcessInput(0, sample.Get(), 0);
        }
        if (FAILED(hr)) {
            std::fprintf(stderr, "viewer: H.264 ProcessInput failed 0x%08lx\n", hr);
            return reset();
        }
        return drain(onPicture) || reset();
    }

private:
    bool create(UINT width, UINT height)
    {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        static const bool started = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
        if (!started || FAILED(CoCreateInstance(CLSID_CMSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&mft_)))) {
            std::fprintf(stderr, "viewer: no H.264 decoder (Media Foundation)\n");
            mft_.Reset();
            return false;
        }
        Microsoft::WRL::ComPtr<IMFAttributes> attributes;
        if (SUCCEEDED(mft_->GetAttributes(&attributes))) attributes->SetUINT32(CODECAPI_AVLowLatencyMode, TRUE);
        Microsoft::WRL::ComPtr<IMFMediaType> input;
        if (FAILED(MFCreateMediaType(&input)) ||
            FAILED(input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) ||
            FAILED(input->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264)) ||
            FAILED(input->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive)) ||
            FAILED(MFSetAttributeSize(input.Get(), MF_MT_FRAME_SIZE, width, height)) ||
            FAILED(MFSetAttributeRatio(input.Get(), MF_MT_FRAME_RATE, 90, 1)) ||
            FAILED(MFSetAttributeRatio(input.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1)) ||
            FAILED(mft_->SetInputType(0, input.Get(), 0)) || !set_output_type()) {
            std::fprintf(stderr, "viewer: H.264 decoder setup failed\n");
            mft_.Reset();
            return false;
        }
        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        std::fprintf(stderr, "viewer: H.264 decoder ready\n");
        return true;
    }

    bool set_output_type()
    {
        Microsoft::WRL::ComPtr<IMFMediaType> type;
        for (DWORD i = 0; SUCCEEDED(mft_->GetOutputAvailableType(0, i, &type)); ++i, type.Reset()) {
            GUID subtype{};
            if (SUCCEEDED(type->GetGUID(MF_MT_SUBTYPE, &subtype)) && subtype == MFVideoFormat_NV12) {
                if (FAILED(mft_->SetOutputType(0, type.Get(), 0))) return false;
                UINT32 width = 0, height = 0, stride = 0;
                MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &width, &height);
                if (FAILED(type->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride)) || static_cast<INT32>(stride) <= 0) stride = width;
                width_ = width; height_ = height; stride_ = stride;
                if (FAILED(mft_->GetOutputStreamInfo(0, &info_))) return false;
                output_.Reset();
                return true;
            }
        }
        return false;
    }

    bool drain(const Callback& onPicture)
    {
        for (;;) {
            const bool provides = (info_.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
            if (!provides && !output_) {
                Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
                const DWORD bytes = std::max<DWORD>(info_.cbSize, DWORD(stride_) * height_ * 3 / 2);
                if (FAILED(MFCreateAlignedMemoryBuffer(bytes, std::max<DWORD>(info_.cbAlignment, 16) - 1, &buffer)) || FAILED(MFCreateSample(&output_)) ||
                    FAILED(output_->AddBuffer(buffer.Get()))) { output_.Reset(); return false; }
            }
            MFT_OUTPUT_DATA_BUFFER out{};
            out.pSample = provides ? nullptr : output_.Get();
            DWORD status = 0;
            const HRESULT hr = mft_->ProcessOutput(0, 1, &out, &status);
            if (out.pEvents) out.pEvents->Release();
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return true;
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
                if (!set_output_type()) return false;
                std::fprintf(stderr, "viewer: H.264 stream %ux%u (pitch %u, sample %lu bytes, align %lu, flags 0x%lx)\n", width_, height_, stride_,
                    info_.cbSize, info_.cbAlignment, info_.dwFlags);
                continue;
            }
            if (FAILED(hr)) {
                std::fprintf(stderr, "viewer: H.264 ProcessOutput failed 0x%08lx (status 0x%lx)\n", hr, out.dwStatus);
                return false;
            }
            Microsoft::WRL::ComPtr<IMFSample> picture;
            picture.Attach(provides ? out.pSample : nullptr);
            if (!provides) picture.Swap(output_);  // A fresh sample for the next picture.
            if (decoded_++ < 3) std::fprintf(stderr, "viewer: H.264 picture %u decoded\n", decoded_);
            Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
            if (FAILED(picture->ConvertToContiguousBuffer(&buffer))) return false;
            Microsoft::WRL::ComPtr<IMF2DBuffer> buffer2d;
            BYTE* bytes = nullptr;
            LONG pitch = 0;
            DWORD length = 0;
            if (SUCCEEDED(buffer.As(&buffer2d)) && SUCCEEDED(buffer2d->Lock2D(&bytes, &pitch)) && pitch > 0) {
                onPicture({bytes, bytes + size_t(pitch) * height_, UINT(pitch), width_, height_});
                buffer2d->Unlock2D();
            } else if (SUCCEEDED(buffer->Lock(&bytes, nullptr, &length))) {
                if (length >= size_t(stride_) * height_ * 3 / 2)
                    onPicture({bytes, bytes + size_t(stride_) * height_, stride_, width_, height_});
                buffer->Unlock();
            }
        }
    }

    // Drops the decoder after an error; the next key frame (every 2 s) starts a new one.
    bool reset()
    {
        mft_.Reset();
        output_.Reset();
        return false;
    }

    Microsoft::WRL::ComPtr<IMFTransform> mft_;
    Microsoft::WRL::ComPtr<IMFSample> output_;
    MFT_OUTPUT_STREAM_INFO info_{};
    UINT width_ = 0, height_ = 0, stride_ = 0;
    unsigned decoded_ = 0;
};
