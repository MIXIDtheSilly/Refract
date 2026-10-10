// Audio: libaaudio.so for the guest and refract.media.HostAudio (behind the shim's
// android.media.AudioTrack / AudioRecord), both on WASAPI shared-mode streams of the default Windows
// playback and recording devices. WASAPI converts rate, channel count and sample format, so every
// stream gets exactly the format the app asked for, and an AAudio data callback writes straight into
// the WASAPI buffer (guest addresses are host addresses).
#include "audio_hle.h"

#include <windows.h>
#include <mmsystem.h>
#include <mmreg.h>
#include <audioclient.h>
#include <mmdeviceapi.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "host_runtime.h"
#include "thunks.h"

namespace rn {

namespace {

// --- AAudio constants (aaudio/AAudio.h) ---------------------------------------------------------

enum : s32 {
    kOk = 0,
    kErrDisconnected = -899,
    kErrIllegalArgument = -898,
    kErrInvalidState = -895,
    kErrInvalidHandle = -892,
    kErrUnavailable = -889,
    kErrTimeout = -885,
    kErrInvalidFormat = -883,
    kErrInvalidRate = -880,
};
enum : s32 { kDirOutput = 0, kDirInput = 1 };
enum : s32 { kFmtUnspecified = 0, kFmtI16 = 1, kFmtFloat = 2, kFmtI24 = 3, kFmtI32 = 4 };
enum : s32 {
    kStateOpen = 2,
    kStateStarting = 3,
    kStateStarted = 4,
    kStatePaused = 6,
    kStateFlushed = 8,
    kStateStopped = 10,
    kStateClosing = 11,
    kStateClosed = 12,
    kStateDisconnected = 13,
};
constexpr s32 kCallbackStop = 1;
constexpr s32 kSharingShared = 1;
// The ids android.media.AudioManager reports for the speaker and the microphone.
constexpr s32 kOutputDeviceId = 2, kInputDeviceId = 1;
constexpr s64 kForever = INT64_MAX / 4;

const GUID kSubtypePcm = {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
const GUID kSubtypeFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

void ComInit() {
    thread_local bool done = false;
    if (!done) {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);  // RPC_E_CHANGED_MODE is fine: WASAPI is free-threaded
        done = true;
    }
}

u32 SampleBytes(s32 format) {
    switch (format) {
    case kFmtI16:
        return 2;
    case kFmtI24:
        return 3;
    default:
        return 4;
    }
}

DWORD ChannelMask(int channels) {
    switch (channels) {
    case 1:
        return 0x4;  // front center
    case 2:
        return 0x3;  // front left/right
    case 4:
        return 0x33;  // quad
    case 6:
        return 0x3f;  // 5.1
    case 8:
        return 0x63f;  // 7.1
    default:
        return 0;
    }
}

IMMDevice* DefaultDevice(bool capture) {
    ComInit();
    IMMDeviceEnumerator* en = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                reinterpret_cast<void**>(&en))))
        return nullptr;
    IMMDevice* dev = nullptr;
    en->GetDefaultAudioEndpoint(capture ? eCapture : eRender, eConsole, &dev);
    en->Release();
    return dev;
}

// The default playback device's mixing rate (what Android reports as the native output rate).
int MixRate() {
    static std::once_flag once;
    static int rate = 48000;
    std::call_once(once, [] {
        IMMDevice* dev = DefaultDevice(false);
        IAudioClient* client = nullptr;
        if (dev && SUCCEEDED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                           reinterpret_cast<void**>(&client)))) {
            WAVEFORMATEX* mix = nullptr;
            if (SUCCEEDED(client->GetMixFormat(&mix))) {
                rate = static_cast<int>(mix->nSamplesPerSec);
                CoTaskMemFree(mix);
            }
            client->Release();
        }
        if (dev)
            dev->Release();
    });
    return rate;
}

// --- streams ---------------------------------------------------------------------------------------

struct Config {
    s32 direction = kDirOutput, sample_rate = 0, channels = 0, format = kFmtUnspecified, sharing = kSharingShared,
        perf = 10 /* NONE */, capacity = 0, frames_per_cb = 0, device_id = 0, session = -1 /* NONE */,
        usage = 1 /* MEDIA */, content = 2 /* MUSIC */, input_preset = 6 /* VOICE_RECOGNITION */,
        allowed_capture = 1, spatialization = 1, spatialized = 0, privacy = 0;
    u32 channel_mask = 0;
    u64 data_cb = 0, data_user = 0, error_cb = 0, error_user = 0;
};

constexpr u32 kStreamMagic = 0x53414152;   // "RAAS"
constexpr u32 kBuilderMagic = 0x42414152;  // "RAAB"

struct Builder {
    u32 magic = kBuilderMagic;
    Config c;
};

struct Stream {
    u32 magic = kStreamMagic;
    Config c;
    bool capture = false;
    IAudioClient* client = nullptr;
    IAudioRenderClient* render = nullptr;
    IAudioCaptureClient* capture_client = nullptr;
    IAudioClock* clock = nullptr;
    HANDLE event = nullptr;
    u32 device_frames = 0;  // WASAPI buffer size
    u32 frame_bytes = 0;
    u32 burst = 0;
    std::atomic<u32> buffer_size{0};
    std::atomic<s32> state{kStateOpen};
    std::atomic<s64> frames_written{0}, frames_read{0};
    std::atomic<s32> xruns{0};
    std::mutex ctl;  // start / stop / close
    std::mutex io;   // blocking read / write and the capture queue
    std::vector<u8> pending;  // captured audio not yet delivered
    size_t pending_off = 0;
    std::vector<u8> cb_buf;
    // Callback mode: a guest worker thread runs the data callback while `running`.
    std::atomic<bool> running{false};
    std::mutex worker_mu;
    std::condition_variable worker_cv;
    bool worker_busy = false;
    bool free_on_exit = false;  // closed from its own callback: the worker frees it

    ~Stream() {
        magic = 0;
        if (clock)
            clock->Release();
        if (render)
            render->Release();
        if (capture_client)
            capture_client->Release();
        if (client)
            client->Release();
        if (event)
            CloseHandle(event);
    }
    size_t PendingBytes() const { return pending.size() - pending_off; }
};

thread_local Stream* tls_worker_stream = nullptr;

Stream* AsStream(Stream* s) { return s && s->magic == kStreamMagic ? s : nullptr; }

s32 OpenStream(const Config& in, Stream** out) {
    *out = nullptr;
    ComInit();
    auto s = std::make_unique<Stream>();
    s->c = in;
    Config& c = s->c;
    s->capture = c.direction == kDirInput;
    if (c.channels <= 0)
        c.channels = s->capture ? 1 : 2;
    if (c.format == kFmtUnspecified)
        c.format = kFmtFloat;
    if (c.format < kFmtI16 || c.format > kFmtI32)
        return kErrInvalidFormat;
    if (c.channels > 8)
        return kErrIllegalArgument;
    if (c.sample_rate <= 0)
        c.sample_rate = MixRate();
    if (c.sample_rate < 4000 || c.sample_rate > 768000)
        return kErrInvalidRate;

    IMMDevice* dev = DefaultDevice(s->capture);
    if (!dev) {
        Log("audio: no default %s device", s->capture ? "recording" : "playback");
        return kErrUnavailable;
    }
    HRESULT hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&s->client));
    dev->Release();
    if (FAILED(hr))
        return kErrUnavailable;

    const u32 bytes = SampleBytes(c.format);
    s->frame_bytes = bytes * static_cast<u32>(c.channels);
    s->burst = c.frames_per_cb > 0 ? static_cast<u32>(c.frames_per_cb) : static_cast<u32>(c.sample_rate) / 100;
    const u32 want = std::max({static_cast<u32>(std::max(c.capacity, 0)), s->burst * 3,
                               static_cast<u32>(c.sample_rate) * 30 / 1000});
    WAVEFORMATEXTENSIBLE f{};
    f.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    f.Format.nChannels = static_cast<WORD>(c.channels);
    f.Format.nSamplesPerSec = static_cast<DWORD>(c.sample_rate);
    f.Format.wBitsPerSample = static_cast<WORD>(bytes * 8);
    f.Format.nBlockAlign = static_cast<WORD>(s->frame_bytes);
    f.Format.nAvgBytesPerSec = f.Format.nSamplesPerSec * f.Format.nBlockAlign;
    f.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    f.Samples.wValidBitsPerSample = f.Format.wBitsPerSample;
    f.dwChannelMask = ChannelMask(c.channels);
    f.SubFormat = c.format == kFmtFloat ? kSubtypeFloat : kSubtypePcm;
    hr = s->client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                               AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                   AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                               static_cast<REFERENCE_TIME>(want) * 10000000 / c.sample_rate, 0, &f.Format, nullptr);
    if (FAILED(hr)) {
        Log("audio: cannot open a %s stream (%d Hz, %d ch, format %d): 0x%08lx", s->capture ? "recording" : "playback",
            c.sample_rate, c.channels, c.format, hr);
        return kErrUnavailable;
    }
    s->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    s->client->SetEventHandle(s->event);
    UINT32 frames = 0;
    s->client->GetBufferSize(&frames);
    s->device_frames = frames;
    if (c.frames_per_cb <= 0 && s->burst * 2 > frames)
        s->burst = std::max(1u, frames / 2);
    // Like AAudio, the capacity is only the limit: start at about two bursts (at least 40 ms, which
    // rides out the JIT's slower callbacks), and the app may change it with setBufferSizeInFrames.
    s->buffer_size = std::min(frames, std::max(s->burst * 2, static_cast<u32>(c.sample_rate) / 25));
    if (s->capture)
        hr = s->client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&s->capture_client));
    else
        hr = s->client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&s->render));
    if (FAILED(hr))
        return kErrUnavailable;
    s->client->GetService(__uuidof(IAudioClock), reinterpret_cast<void**>(&s->clock));
    static std::atomic<s32> next_session{1000};
    if (c.session == 0)
        c.session = next_session++;
    c.device_id = s->capture ? kInputDeviceId : kOutputDeviceId;
    c.sharing = kSharingShared;
    s->cb_buf.resize(static_cast<size_t>(s->burst) * s->frame_bytes);
    RN_INFO("audio: %s stream %d Hz, %d ch, format %d, %u frames per burst, buffer %u of %u frames%s",
            s->capture ? "recording" : "playback", c.sample_rate, c.channels, c.format, s->burst,
            s->buffer_size.load(), frames, c.data_cb ? ", callback" : "");
    *out = s.release();
    return kOk;
}

// Moves everything WASAPI has captured into s->pending (s->io held). False if the device is gone.
bool DrainCapture(Stream* s) {
    for (;;) {
        UINT32 packet = 0;
        if (FAILED(s->capture_client->GetNextPacketSize(&packet)))
            return false;
        if (packet == 0)
            return true;
        BYTE* data = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        HRESULT hr = s->capture_client->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
        if (FAILED(hr))
            return false;
        if (hr == AUDCLNT_S_BUFFER_EMPTY)
            return true;
        const size_t n = static_cast<size_t>(frames) * s->frame_bytes;
        const size_t old = s->pending.size();
        s->pending.resize(old + n);
        if (flags & AUDCLNT_BUFFERFLAGS_SILENT)
            memset(s->pending.data() + old, 0, n);
        else
            memcpy(s->pending.data() + old, data, n);
        s->capture_client->ReleaseBuffer(frames);
        // Keep at most a second when nobody reads.
        const size_t max = static_cast<size_t>(s->c.sample_rate) * s->frame_bytes;
        if (s->PendingBytes() > max) {
            s->pending_off = s->pending.size() - max;
            s->xruns++;
        }
    }
}

void CompactPending(Stream* s) {
    if (s->pending_off == s->pending.size()) {
        s->pending.clear();
        s->pending_off = 0;
    } else if (s->pending_off > 65536) {
        s->pending.erase(s->pending.begin(), s->pending.begin() + static_cast<ptrdiff_t>(s->pending_off));
        s->pending_off = 0;
    }
}

u32 WriteLimit(Stream* s) {
    return std::min<u32>(s->buffer_size.load(), s->device_frames);
}

// --- callback mode ---------------------------------------------------------------------------------

// Host threads adopted as guest threads, reused across streams (an adopted thread lives forever).
class GuestWorkers {
public:
    void Post(std::function<void(GuestThread*)> job) {
        std::lock_guard lock(mu_);
        jobs_.push_back(std::move(job));
        if (jobs_.size() > idle_) {
            ++idle_;
            std::thread([this] { Loop(); }).detach();
        }
        cv_.notify_one();
    }

private:
    void Loop() {
        ComInit();
        GuestThread* t = EnsureGuestThread();
        t->name = "AAudio";
        std::unique_lock lock(mu_);
        for (;;) {
            cv_.wait(lock, [&] { return !jobs_.empty(); });
            auto job = std::move(jobs_.front());
            jobs_.pop_front();
            --idle_;
            lock.unlock();
            job(t);
            lock.lock();
            ++idle_;
        }
    }
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::function<void(GuestThread*)>> jobs_;
    size_t idle_ = 0;
};

GuestWorkers& Workers() {
    static GuestWorkers* w = new GuestWorkers;
    return *w;
}

s32 CallData(GuestThread* t, Stream* s, void* data, u32 frames) {
    return static_cast<s32>(CallGuest(t, s->c.data_cb, {reinterpret_cast<u64>(s), s->c.data_user,
                                                       reinterpret_cast<u64>(data), frames}));
}

bool OutputCallbacks(Stream* s, GuestThread* t, bool* stopped) {
    bool started = false;
    while (s->running) {
        UINT32 padding = 0;
        if (FAILED(s->client->GetCurrentPadding(&padding)))
            return false;
        if (started && padding == 0)
            s->xruns++;
        while (s->running && padding + s->burst <= WriteLimit(s)) {
            BYTE* data = nullptr;
            if (FAILED(s->render->GetBuffer(s->burst, &data)))
                return false;
            const s32 r = CallData(t, s, data, s->burst);
            s->render->ReleaseBuffer(s->burst, 0);
            s->frames_written += s->burst;
            padding += s->burst;
            if (r == kCallbackStop) {
                s->running = false;
                *stopped = true;
            }
        }
        if (!started) {
            if (FAILED(s->client->Start()))
                return false;
            started = true;
        }
        WaitForSingleObject(s->event, 100);
    }
    return true;
}

bool InputCallbacks(Stream* s, GuestThread* t, bool* stopped) {
    if (FAILED(s->client->Start()))
        return false;
    const size_t chunk = s->cb_buf.size();
    while (s->running) {
        WaitForSingleObject(s->event, 100);
        std::unique_lock lock(s->io);
        if (!DrainCapture(s))
            return false;
        while (s->running && s->PendingBytes() >= chunk) {
            memcpy(s->cb_buf.data(), s->pending.data() + s->pending_off, chunk);
            s->pending_off += chunk;
            lock.unlock();
            const s32 r = CallData(t, s, s->cb_buf.data(), s->burst);
            s->frames_read += s->burst;
            lock.lock();
            if (r == kCallbackStop) {
                s->running = false;
                *stopped = true;
            }
        }
        CompactPending(s);
    }
    return true;
}

void RunCallbacks(Stream* s, GuestThread* t) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    tls_worker_stream = s;
    bool stopped = false;
    const bool ok = s->capture ? InputCallbacks(s, t, &stopped) : OutputCallbacks(s, t, &stopped);
    s->client->Stop();
    if (!ok) {
        Log("audio: %s device lost", s->capture ? "recording" : "playback");
        s->running = false;
        s->state = kStateDisconnected;
        if (s->c.error_cb)
            CallGuest(t, s->c.error_cb, {reinterpret_cast<u64>(s), s->c.error_user,
                                         static_cast<u64>(static_cast<s64>(kErrDisconnected))});
    } else if (stopped) {
        s->state = kStateStopped;
    }
    tls_worker_stream = nullptr;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
    bool free_it;
    {
        std::lock_guard lock(s->worker_mu);
        s->worker_busy = false;
        free_it = s->free_on_exit;
        s->worker_cv.notify_all();
    }
    if (free_it)
        delete s;
}

void WaitWorker(Stream* s) {
    std::unique_lock lock(s->worker_mu);
    s->worker_cv.wait(lock, [&] { return !s->worker_busy; });
}

// --- control ---------------------------------------------------------------------------------------

s32 Start(Stream* s) {
    std::lock_guard lock(s->ctl);
    const s32 st = s->state;
    if (st == kStateDisconnected)
        return kErrDisconnected;
    if (st == kStateClosing || st == kStateClosed)
        return kErrInvalidState;
    if (st == kStateStarted || st == kStateStarting)
        return kOk;
    if (s->c.data_cb) {
        WaitWorker(s);  // a run its callback stopped
        s->running = true;
        {
            std::lock_guard wl(s->worker_mu);
            s->worker_busy = true;
        }
        s->state = kStateStarted;
        Workers().Post([s](GuestThread* t) { RunCallbacks(s, t); });
        return kOk;
    }
    const HRESULT hr = s->client->Start();
    if (FAILED(hr) && hr != AUDCLNT_E_NOT_STOPPED) {
        s->state = kStateDisconnected;
        return kErrDisconnected;
    }
    s->state = kStateStarted;
    return kOk;
}

// Stops the stream (pause and stop differ only in the state they leave).
s32 Halt(Stream* s, s32 final_state) {
    if (tls_worker_stream == s) {  // from its own callback: the worker stops the client on its way out
        s->running = false;
        s->state = final_state;
        return kOk;
    }
    std::lock_guard lock(s->ctl);
    s->running = false;
    SetEvent(s->event);
    WaitWorker(s);
    s->client->Stop();
    if (s->state != kStateDisconnected)
        s->state = final_state;
    if (s->capture && final_state == kStateStopped) {
        std::lock_guard io(s->io);
        DrainCapture(s);
        s->pending.clear();
        s->pending_off = 0;
    }
    return kOk;
}

s32 Flush(Stream* s) {
    std::lock_guard lock(s->ctl);
    if (s->capture)
        return kErrIllegalArgument;
    const s32 st = s->state;
    if (st != kStatePaused && st != kStateStopped && st != kStateOpen && st != kStateFlushed)
        return kErrInvalidState;
    s->client->Reset();
    s->state = kStateFlushed;
    return kOk;
}

s32 Close(Stream* s) {
    if (tls_worker_stream == s) {
        s->running = false;
        s->state = kStateClosed;
        std::lock_guard lock(s->worker_mu);
        s->free_on_exit = true;
        return kOk;
    }
    Halt(s, kStateStopped);
    {
        std::lock_guard io(s->io);  // a blocking read/write in progress times out
    }
    s->state = kStateClosed;
    delete s;
    return kOk;
}

// --- blocking read / write -------------------------------------------------------------------------

void WaitIo(Stream* s, u64 deadline) {
    const u64 now = MonotonicNs();
    WaitForSingleObject(s->event, static_cast<DWORD>(std::min<u64>((deadline - now) / 1000000 + 1, 20)));
}

s64 Write(Stream* s, const u8* src, s64 frames, s64 timeout_ns) {
    if (s->capture || s->c.data_cb)
        return kErrInvalidState;
    if (s->state == kStateDisconnected)
        return kErrDisconnected;
    std::lock_guard lock(s->io);
    const u64 deadline = MonotonicNs() + static_cast<u64>(std::max<s64>(timeout_ns, 0));
    s64 done = 0;
    while (done < frames) {
        UINT32 padding = 0;
        BYTE* data = nullptr;
        if (FAILED(s->client->GetCurrentPadding(&padding))) {
            s->state = kStateDisconnected;
            return done ? done : kErrDisconnected;
        }
        const u32 limit = WriteLimit(s);
        if (padding < limit) {
            const u32 n = static_cast<u32>(std::min<s64>(limit - padding, frames - done));
            if (FAILED(s->render->GetBuffer(n, &data))) {
                s->state = kStateDisconnected;
                return done ? done : kErrDisconnected;
            }
            memcpy(data, src + done * s->frame_bytes, static_cast<size_t>(n) * s->frame_bytes);
            s->render->ReleaseBuffer(n, 0);
            done += n;
            s->frames_written += n;
            continue;
        }
        if (timeout_ns <= 0 || MonotonicNs() >= deadline || s->state != kStateStarted)
            break;
        WaitIo(s, deadline);
    }
    return done;
}

s64 Read(Stream* s, u8* dst, s64 frames, s64 timeout_ns) {
    if (!s->capture || s->c.data_cb)
        return kErrInvalidState;
    if (s->state == kStateDisconnected)
        return kErrDisconnected;
    std::lock_guard lock(s->io);
    const u64 deadline = MonotonicNs() + static_cast<u64>(std::max<s64>(timeout_ns, 0));
    s64 done = 0;
    while (done < frames) {
        if (!DrainCapture(s)) {
            s->state = kStateDisconnected;
            return done ? done : kErrDisconnected;
        }
        const s64 have = static_cast<s64>(s->PendingBytes() / s->frame_bytes);
        if (have) {
            const s64 n = std::min(have, frames - done);
            const size_t bytes = static_cast<size_t>(n) * s->frame_bytes;
            memcpy(dst + done * s->frame_bytes, s->pending.data() + s->pending_off, bytes);
            s->pending_off += bytes;
            done += n;
            s->frames_read += n;
            continue;
        }
        if (timeout_ns <= 0 || MonotonicNs() >= deadline || s->state != kStateStarted)
            break;
        WaitIo(s, deadline);
    }
    CompactPending(s);
    return done;
}

// Frames the device has played (output) or captured (input) so far, and when (CLOCK_MONOTONIC ns).
void DevicePosition(Stream* s, s64* frames, s64* ns) {
    UINT64 freq = 0, pos = 0, qpc = 0;
    if (s->clock && SUCCEEDED(s->clock->GetFrequency(&freq)) && freq &&
        SUCCEEDED(s->clock->GetPosition(&pos, &qpc))) {
        *frames = static_cast<s64>(static_cast<double>(pos) * s->c.sample_rate / static_cast<double>(freq));
        *ns = static_cast<s64>(qpc * 100);  // QPC time in 100 ns units, MonotonicNs's clock
        return;
    }
    UINT32 padding = 0;
    if (!s->capture)
        s->client->GetCurrentPadding(&padding);
    *frames = s->capture ? s->frames_read.load() : s->frames_written.load() - padding;
    *ns = static_cast<s64>(MonotonicNs());
}

// --- libaaudio.so ----------------------------------------------------------------------------------

Builder* AsBuilder(Builder* b) { return b && b->magic == kBuilderMagic ? b : nullptr; }

s32 A_createStreamBuilder(u64 out) {
    u64 p = reinterpret_cast<u64>(new Builder);
    SafeCopyToGuest(out, &p, 8);
    return kOk;
}
s32 A_builderDelete(Builder* b) {
    if (!AsBuilder(b))
        return kErrInvalidHandle;
    b->magic = 0;
    delete b;
    return kOk;
}
s32 A_openStream(Builder* b, u64 out) {
    if (!AsBuilder(b))
        return kErrInvalidHandle;
    Stream* s = nullptr;
    const s32 r = OpenStream(b->c, &s);
    u64 p = reinterpret_cast<u64>(s);
    SafeCopyToGuest(out, &p, 8);
    return r;
}
template <s32 Config::*F>
void A_set(Builder* b, s32 v) {
    if (AsBuilder(b))
        b->c.*F = v;
}
void A_setPrivacy(Builder* b, s32 v) {
    if (AsBuilder(b))
        b->c.privacy = v & 0xff ? 1 : 0;
}
void A_setSpatialized(Builder* b, s32 v) {
    if (AsBuilder(b))
        b->c.spatialized = v & 0xff ? 1 : 0;
}
void A_setChannelMask(Builder* b, u32 mask) {
    if (AsBuilder(b)) {
        b->c.channel_mask = mask;
        b->c.channels = std::popcount(mask);
    }
}
void A_setDataCallback(Builder* b, u64 fn, u64 user) {
    if (AsBuilder(b)) {
        b->c.data_cb = fn;
        b->c.data_user = user;
    }
}
void A_setErrorCallback(Builder* b, u64 fn, u64 user) {
    if (AsBuilder(b)) {
        b->c.error_cb = fn;
        b->c.error_user = user;
    }
}
void A_ignore(Builder*, u64) {}

template <s32 Config::*F>
s32 A_get(Stream* s) {
    return AsStream(s) ? s->c.*F : kErrInvalidHandle;
}
s32 A_getChannelMask(Stream* s) {
    if (!AsStream(s))
        return 0;
    if (s->c.channel_mask)
        return static_cast<s32>(s->c.channel_mask);
    return s->c.channels == 1 ? 1 : static_cast<s32>((1u << s->c.channels) - 1);
}
s32 A_requestStart(Stream* s) { return AsStream(s) ? Start(s) : kErrInvalidHandle; }
s32 A_requestStop(Stream* s) { return AsStream(s) ? Halt(s, kStateStopped) : kErrInvalidHandle; }
s32 A_requestPause(Stream* s) {
    if (!AsStream(s))
        return kErrInvalidHandle;
    return s->capture ? kErrIllegalArgument : Halt(s, kStatePaused);
}
s32 A_requestFlush(Stream* s) { return AsStream(s) ? Flush(s) : kErrInvalidHandle; }
s32 A_release(Stream* s) {
    if (!AsStream(s))
        return kErrInvalidHandle;
    Halt(s, kStateStopped);
    s->state = kStateClosing;
    return kOk;
}
s32 A_close(Stream* s) { return AsStream(s) ? Close(s) : kErrInvalidHandle; }
s32 A_getState(Stream* s) { return AsStream(s) ? s->state.load() : kStateClosed; }
s32 A_waitForStateChange(Stream* s, s32 input, u64 next, s64 timeout_ns) {
    if (!AsStream(s))
        return kErrInvalidHandle;
    const u64 deadline = MonotonicNs() + static_cast<u64>(std::max<s64>(timeout_ns, 0));
    s32 st;
    while ((st = s->state) == input && MonotonicNs() < deadline)
        Sleep(1);
    if (next)
        SafeCopyToGuest(next, &st, 4);
    return st == input ? kErrTimeout : kOk;
}
s32 A_read(Stream* s, u64 buffer, s32 frames, s64 timeout_ns) {
    if (!AsStream(s))
        return kErrInvalidHandle;
    return static_cast<s32>(Read(s, reinterpret_cast<u8*>(buffer), frames, timeout_ns));
}
s32 A_write(Stream* s, u64 buffer, s32 frames, s64 timeout_ns) {
    if (!AsStream(s))
        return kErrInvalidHandle;
    return static_cast<s32>(Write(s, reinterpret_cast<const u8*>(buffer), frames, timeout_ns));
}
s32 A_getBufferSize(Stream* s) { return AsStream(s) ? static_cast<s32>(s->buffer_size.load()) : kErrInvalidHandle; }
s32 A_setBufferSize(Stream* s, s32 frames) {
    if (!AsStream(s))
        return kErrInvalidHandle;
    const u32 lo = std::min(s->burst * 2, s->device_frames);
    s->buffer_size = std::clamp<u32>(static_cast<u32>(std::max(frames, 0)), lo, s->device_frames);
    return static_cast<s32>(s->buffer_size.load());
}
s32 A_getBufferCapacity(Stream* s) { return AsStream(s) ? static_cast<s32>(s->device_frames) : kErrInvalidHandle; }
s32 A_getFramesPerBurst(Stream* s) { return AsStream(s) ? static_cast<s32>(s->burst) : kErrInvalidHandle; }
s32 A_getXRunCount(Stream* s) { return AsStream(s) ? s->xruns.load() : kErrInvalidHandle; }
s32 A_getHardwareSampleRate(Stream*) { return MixRate(); }
s32 A_getHardwareFormat(Stream*) { return kFmtFloat; }
s32 A_isMMapUsed(Stream*) { return 0; }
s64 A_getFramesWritten(Stream* s) {
    if (!AsStream(s))
        return kErrInvalidHandle;
    if (!s->capture)
        return s->frames_written;
    std::lock_guard lock(s->io);
    return s->frames_read + static_cast<s64>(s->PendingBytes() / s->frame_bytes);
}
s64 A_getFramesRead(Stream* s) {
    if (!AsStream(s))
        return kErrInvalidHandle;
    if (s->capture)
        return s->frames_read;
    s64 frames, ns;
    DevicePosition(s, &frames, &ns);
    return std::min(frames, s->frames_written.load());
}
s32 A_getTimestamp(Stream* s, s32 /*clockid*/, u64 frame_pos, u64 time_ns) {
    if (!AsStream(s))
        return kErrInvalidHandle;
    if (s->state != kStateStarted)
        return kErrInvalidState;
    s64 frames, ns;
    DevicePosition(s, &frames, &ns);
    if (frame_pos)
        SafeCopyToGuest(frame_pos, &frames, 8);
    if (time_ns)
        SafeCopyToGuest(time_ns, &ns, 8);
    return kOk;
}

const char* A_resultText(s32 r) {
    switch (r) {
    case kOk:
        return "AAUDIO_OK";
    case kErrDisconnected:
        return "AAUDIO_ERROR_DISCONNECTED";
    case kErrIllegalArgument:
        return "AAUDIO_ERROR_ILLEGAL_ARGUMENT";
    case kErrInvalidState:
        return "AAUDIO_ERROR_INVALID_STATE";
    case kErrInvalidHandle:
        return "AAUDIO_ERROR_INVALID_HANDLE";
    case kErrUnavailable:
        return "AAUDIO_ERROR_UNAVAILABLE";
    case kErrTimeout:
        return "AAUDIO_ERROR_TIMEOUT";
    case kErrInvalidFormat:
        return "AAUDIO_ERROR_INVALID_FORMAT";
    case kErrInvalidRate:
        return "AAUDIO_ERROR_INVALID_RATE";
    default:
        return "AAUDIO_ERROR_UNKNOWN";
    }
}
const char* A_stateText(s32 st) {
    static const char* const kNames[] = {
        "AAUDIO_STREAM_STATE_UNINITIALIZED", "AAUDIO_STREAM_STATE_UNKNOWN",  "AAUDIO_STREAM_STATE_OPEN",
        "AAUDIO_STREAM_STATE_STARTING",      "AAUDIO_STREAM_STATE_STARTED",  "AAUDIO_STREAM_STATE_PAUSING",
        "AAUDIO_STREAM_STATE_PAUSED",        "AAUDIO_STREAM_STATE_FLUSHING", "AAUDIO_STREAM_STATE_FLUSHED",
        "AAUDIO_STREAM_STATE_STOPPING",      "AAUDIO_STREAM_STATE_STOPPED",  "AAUDIO_STREAM_STATE_CLOSING",
        "AAUDIO_STREAM_STATE_CLOSED",        "AAUDIO_STREAM_STATE_DISCONNECTED",
    };
    return st >= 0 && st < 14 ? kNames[st] : "AAUDIO_STREAM_STATE_UNKNOWN";
}
s32 A_getMMapPolicy() { return 1; }  // AAUDIO_POLICY_NEVER
s32 A_setMMapPolicy(s32) { return kOk; }

template <auto F>
ThunkFn W() {
    return &Wrap<F>;
}

// --- refract.media.HostAudio (android.media.AudioTrack / AudioRecord) ------------------------------

Stream* JStream(jlong h) { return AsStream(reinterpret_cast<Stream*>(h)); }

jlong JNICALL J_open(JNIEnv*, jclass, jboolean capture, jint rate, jint channels, jint format, jint buffer_frames) {
    Config c;
    c.direction = capture ? kDirInput : kDirOutput;
    c.sample_rate = rate;
    c.channels = channels;
    c.format = format;
    c.capacity = buffer_frames;
    c.input_preset = 7;  // VOICE_COMMUNICATION
    Stream* s = nullptr;
    return OpenStream(c, &s) == kOk ? reinterpret_cast<jlong>(s) : 0;
}
jint JNICALL J_start(JNIEnv*, jclass, jlong h) {
    Stream* s = JStream(h);
    return s ? Start(s) : kErrInvalidHandle;
}
void JNICALL J_stop(JNIEnv*, jclass, jlong h) {
    if (Stream* s = JStream(h))
        Halt(s, kStateStopped);
}
void JNICALL J_pause(JNIEnv*, jclass, jlong h) {
    if (Stream* s = JStream(h))
        Halt(s, kStatePaused);
}
void JNICALL J_flush(JNIEnv*, jclass, jlong h) {
    if (Stream* s = JStream(h))
        Flush(s);
}
void JNICALL J_close(JNIEnv*, jclass, jlong h) {
    if (Stream* s = JStream(h))
        Close(s);
}
jint JNICALL J_bufferFrames(JNIEnv*, jclass, jlong h) {
    Stream* s = JStream(h);
    return s ? static_cast<jint>(s->device_frames) : 0;
}
jlong JNICALL J_position(JNIEnv*, jclass, jlong h) {
    Stream* s = JStream(h);
    if (!s)
        return 0;
    if (s->capture)
        return s->frames_read;
    UINT32 padding = 0;
    s->client->GetCurrentPadding(&padding);
    return s->frames_written - padding;
}
jint JNICALL J_underruns(JNIEnv*, jclass, jlong h) {
    Stream* s = JStream(h);
    return s ? s->xruns.load() : 0;
}
jint JNICALL J_mixRate(JNIEnv*, jclass) { return MixRate(); }

// Byte counts in and out; negative results are AAudio errors.
jint JNICALL J_writeArray(JNIEnv* env, jclass, jlong h, jobject array, jint off, jint bytes, jboolean blocking) {
    Stream* s = JStream(h);
    if (!s || !array || off < 0 || bytes < 0)
        return kErrIllegalArgument;
    const s64 frames = bytes / s->frame_bytes;
    std::vector<u8> tmp(static_cast<size_t>(frames) * s->frame_bytes);
    void* p = env->GetPrimitiveArrayCritical(static_cast<jarray>(array), nullptr);
    if (!p)
        return kErrIllegalArgument;
    memcpy(tmp.data(), static_cast<u8*>(p) + off, tmp.size());
    env->ReleasePrimitiveArrayCritical(static_cast<jarray>(array), p, JNI_ABORT);
    const s64 n = Write(s, tmp.data(), frames, blocking ? kForever : 0);
    return n < 0 ? static_cast<jint>(n) : static_cast<jint>(n * s->frame_bytes);
}
jint JNICALL J_writeBuffer(JNIEnv* env, jclass, jlong h, jobject buf, jint off, jint bytes, jboolean blocking) {
    Stream* s = JStream(h);
    u8* p = buf ? static_cast<u8*>(env->GetDirectBufferAddress(buf)) : nullptr;
    if (!s || !p || off < 0 || bytes < 0)
        return kErrIllegalArgument;
    const s64 n = Write(s, p + off, bytes / s->frame_bytes, blocking ? kForever : 0);
    return n < 0 ? static_cast<jint>(n) : static_cast<jint>(n * s->frame_bytes);
}
jint JNICALL J_readArray(JNIEnv* env, jclass, jlong h, jobject array, jint off, jint bytes, jboolean blocking) {
    Stream* s = JStream(h);
    if (!s || !array || off < 0 || bytes < 0)
        return kErrIllegalArgument;
    std::vector<u8> tmp(static_cast<size_t>(bytes / s->frame_bytes) * s->frame_bytes);
    const s64 n = Read(s, tmp.data(), bytes / s->frame_bytes, blocking ? kForever : 0);
    if (n <= 0)
        return static_cast<jint>(n);
    void* p = env->GetPrimitiveArrayCritical(static_cast<jarray>(array), nullptr);
    if (!p)
        return kErrIllegalArgument;
    memcpy(static_cast<u8*>(p) + off, tmp.data(), static_cast<size_t>(n) * s->frame_bytes);
    env->ReleasePrimitiveArrayCritical(static_cast<jarray>(array), p, 0);
    return static_cast<jint>(n * s->frame_bytes);
}
jint JNICALL J_readBuffer(JNIEnv* env, jclass, jlong h, jobject buf, jint off, jint bytes, jboolean blocking) {
    Stream* s = JStream(h);
    u8* p = buf ? static_cast<u8*>(env->GetDirectBufferAddress(buf)) : nullptr;
    if (!s || !p || off < 0 || bytes < 0)
        return kErrIllegalArgument;
    const s64 n = Read(s, p + off, bytes / s->frame_bytes, blocking ? kForever : 0);
    return n < 0 ? static_cast<jint>(n) : static_cast<jint>(n * s->frame_bytes);
}

}  // namespace

bool RegisterAudioNatives(JNIEnv* env) {
    jclass c = env->FindClass("refract/media/HostAudio");
    if (!c) {
        env->ExceptionClear();
        Log("refract.media.HostAudio missing: no AudioTrack/AudioRecord");
        return false;
    }
    auto m = [](const char* name, const char* sig, auto fn) {
        return JNINativeMethod{const_cast<char*>(name), const_cast<char*>(sig), reinterpret_cast<void*>(fn)};
    };
    const JNINativeMethod methods[] = {
        m("open", "(ZIIII)J", &J_open),
        m("start", "(J)I", &J_start),
        m("stop", "(J)V", &J_stop),
        m("pause", "(J)V", &J_pause),
        m("flush", "(J)V", &J_flush),
        m("close", "(J)V", &J_close),
        m("bufferFrames", "(J)I", &J_bufferFrames),
        m("position", "(J)J", &J_position),
        m("underruns", "(J)I", &J_underruns),
        m("mixRate", "()I", &J_mixRate),
        m("writeArray", "(JLjava/lang/Object;IIZ)I", &J_writeArray),
        m("writeBuffer", "(JLjava/nio/ByteBuffer;IIZ)I", &J_writeBuffer),
        m("readArray", "(JLjava/lang/Object;IIZ)I", &J_readArray),
        m("readBuffer", "(JLjava/nio/ByteBuffer;IIZ)I", &J_readBuffer),
    };
    return env->RegisterNatives(c, methods, sizeof(methods) / sizeof(methods[0])) == JNI_OK;
}

void RegisterAudioHle() {
    RegisterHostLibrary(
        kLibAAudio, "libaaudio.so",
        {
            {"AAudio_createStreamBuilder", W<&A_createStreamBuilder>()},
            {"AAudioStreamBuilder_delete", W<&A_builderDelete>()},
            {"AAudioStreamBuilder_openStream", W<&A_openStream>()},
            {"AAudioStreamBuilder_setDirection", W<&A_set<&Config::direction>>()},
            {"AAudioStreamBuilder_setSampleRate", W<&A_set<&Config::sample_rate>>()},
            {"AAudioStreamBuilder_setChannelCount", W<&A_set<&Config::channels>>()},
            {"AAudioStreamBuilder_setSamplesPerFrame", W<&A_set<&Config::channels>>()},
            {"AAudioStreamBuilder_setChannelMask", W<&A_setChannelMask>()},
            {"AAudioStreamBuilder_setFormat", W<&A_set<&Config::format>>()},
            {"AAudioStreamBuilder_setSharingMode", W<&A_set<&Config::sharing>>()},
            {"AAudioStreamBuilder_setPerformanceMode", W<&A_set<&Config::perf>>()},
            {"AAudioStreamBuilder_setBufferCapacityInFrames", W<&A_set<&Config::capacity>>()},
            {"AAudioStreamBuilder_setFramesPerDataCallback", W<&A_set<&Config::frames_per_cb>>()},
            {"AAudioStreamBuilder_setDeviceId", W<&A_set<&Config::device_id>>()},
            {"AAudioStreamBuilder_setSessionId", W<&A_set<&Config::session>>()},
            {"AAudioStreamBuilder_setUsage", W<&A_set<&Config::usage>>()},
            {"AAudioStreamBuilder_setContentType", W<&A_set<&Config::content>>()},
            {"AAudioStreamBuilder_setInputPreset", W<&A_set<&Config::input_preset>>()},
            {"AAudioStreamBuilder_setAllowedCapturePolicy", W<&A_set<&Config::allowed_capture>>()},
            {"AAudioStreamBuilder_setSpatializationBehavior", W<&A_set<&Config::spatialization>>()},
            {"AAudioStreamBuilder_setIsContentSpatialized", W<&A_setSpatialized>()},
            {"AAudioStreamBuilder_setPrivacySensitive", W<&A_setPrivacy>()},
            {"AAudioStreamBuilder_setPackageName", W<&A_ignore>()},
            {"AAudioStreamBuilder_setAttributionTag", W<&A_ignore>()},
            {"AAudioStreamBuilder_setDataCallback", W<&A_setDataCallback>()},
            {"AAudioStreamBuilder_setErrorCallback", W<&A_setErrorCallback>()},
            {"AAudioStream_requestStart", W<&A_requestStart>()},
            {"AAudioStream_requestPause", W<&A_requestPause>()},
            {"AAudioStream_requestFlush", W<&A_requestFlush>()},
            {"AAudioStream_requestStop", W<&A_requestStop>()},
            {"AAudioStream_release", W<&A_release>()},
            {"AAudioStream_close", W<&A_close>()},
            {"AAudioStream_getState", W<&A_getState>()},
            {"AAudioStream_waitForStateChange", W<&A_waitForStateChange>()},
            {"AAudioStream_read", W<&A_read>()},
            {"AAudioStream_write", W<&A_write>()},
            {"AAudioStream_getBufferSizeInFrames", W<&A_getBufferSize>()},
            {"AAudioStream_setBufferSizeInFrames", W<&A_setBufferSize>()},
            {"AAudioStream_getBufferCapacityInFrames", W<&A_getBufferCapacity>()},
            {"AAudioStream_getFramesPerBurst", W<&A_getFramesPerBurst>()},
            {"AAudioStream_getFramesPerDataCallback", W<&A_get<&Config::frames_per_cb>>()},
            {"AAudioStream_getXRunCount", W<&A_getXRunCount>()},
            {"AAudioStream_getSampleRate", W<&A_get<&Config::sample_rate>>()},
            {"AAudioStream_getHardwareSampleRate", W<&A_getHardwareSampleRate>()},
            {"AAudioStream_getChannelCount", W<&A_get<&Config::channels>>()},
            {"AAudioStream_getSamplesPerFrame", W<&A_get<&Config::channels>>()},
            {"AAudioStream_getHardwareChannelCount", W<&A_get<&Config::channels>>()},
            {"AAudioStream_getChannelMask", W<&A_getChannelMask>()},
            {"AAudioStream_getFormat", W<&A_get<&Config::format>>()},
            {"AAudioStream_getHardwareFormat", W<&A_getHardwareFormat>()},
            {"AAudioStream_getSharingMode", W<&A_get<&Config::sharing>>()},
            {"AAudioStream_getPerformanceMode", W<&A_get<&Config::perf>>()},
            {"AAudioStream_getDirection", W<&A_get<&Config::direction>>()},
            {"AAudioStream_getDeviceId", W<&A_get<&Config::device_id>>()},
            {"AAudioStream_getSessionId", W<&A_get<&Config::session>>()},
            {"AAudioStream_getUsage", W<&A_get<&Config::usage>>()},
            {"AAudioStream_getContentType", W<&A_get<&Config::content>>()},
            {"AAudioStream_getInputPreset", W<&A_get<&Config::input_preset>>()},
            {"AAudioStream_getAllowedCapturePolicy", W<&A_get<&Config::allowed_capture>>()},
            {"AAudioStream_getSpatializationBehavior", W<&A_get<&Config::spatialization>>()},
            {"AAudioStream_isContentSpatialized", W<&A_get<&Config::spatialized>>()},
            {"AAudioStream_isPrivacySensitive", W<&A_get<&Config::privacy>>()},
            {"AAudioStream_isMMapUsed", W<&A_isMMapUsed>()},
            {"AAudioStream_getFramesWritten", W<&A_getFramesWritten>()},
            {"AAudioStream_getFramesRead", W<&A_getFramesRead>()},
            {"AAudioStream_getTimestamp", W<&A_getTimestamp>()},
            {"AAudio_convertResultToText", W<&A_resultText>()},
            {"AAudio_convertStreamStateToText", W<&A_stateText>()},
            {"AAudio_getMMapPolicy", W<&A_getMMapPolicy>()},
            {"AAudio_setMMapPolicy", W<&A_setMMapPolicy>()},
        });
}

}  // namespace rn
