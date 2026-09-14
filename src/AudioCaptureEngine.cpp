#include "AudioCaptureEngine.h"

#include <avrt.h>
#include <functiondiscoverykeys_devpkey.h>

#include <cstdio>
#include <cstring>

namespace
{
// Optional diagnostic trace: SIMPLERECORDER_TRACE = file path.
void Trace(const std::wstring& text)
{
    wchar_t path[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"SIMPLERECORDER_TRACE", path, MAX_PATH) == 0)
    {
        return;
    }

    FILE* file = nullptr;
    _wfopen_s(&file, path, L"a, ccs=UTF-8");
    if (file == nullptr)
    {
        return;
    }
    fwprintf(file, L"%s\n", text.c_str());
    fclose(file);
}

std::wstring HrText(HRESULT hr)
{
    wchar_t buffer[64];
    swprintf_s(buffer, L"0x%08X", static_cast<unsigned int>(hr));
    return buffer;
}

// Maximum gap that is turned into silence in one step (guards against a bogus
// timestamp producing a gigantic file).
const UINT64 kMaxGapFrames = 48000ULL * 600; // 10 minutes @ 48 kHz
} // namespace

AudioCaptureEngine::~AudioCaptureEngine()
{
    Close();
}

void AudioCaptureEngine::Close()
{
    Stop();

    if (m_captureClient)
    {
        m_captureClient->Release();
        m_captureClient = nullptr;
    }
    if (m_audioClient)
    {
        m_audioClient->Release();
        m_audioClient = nullptr;
    }
    if (m_waveFormat)
    {
        CoTaskMemFree(m_waveFormat);
        m_waveFormat = nullptr;
    }
    if (m_device)
    {
        m_device->Release();
        m_device = nullptr;
    }
    if (m_enumerator)
    {
        m_enumerator->Release();
        m_enumerator = nullptr;
    }
}

std::wstring AudioCaptureEngine::DescribeFormat() const
{
    if (!m_waveFormat)
    {
        return L"(未知格式)";
    }
    wchar_t buffer[160];
    swprintf_s(buffer, L"%u Hz / %u ch / %u bit",
               m_waveFormat->nSamplesPerSec,
               m_waveFormat->nChannels,
               m_waveFormat->wBitsPerSample);
    return buffer;
}

bool AudioCaptureEngine::Open(CaptureRoute route, const std::wstring& tempWavPath, std::wstring& error)
{
    m_wavPath = tempWavPath;

    const bool loopback = (route == CaptureRoute::SystemLoopback);
    const EDataFlow flow = loopback ? eRender : eCapture;
    const wchar_t* what = loopback ? L"系统声音（默认输出设备）" : L"麦克风（默认输入设备）";

    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void**>(&m_enumerator));
    if (FAILED(hr))
    {
        error = L"无法创建音频设备枚举器 (";
        error += HrText(hr);
        error += L")";
        return false;
    }

    hr = m_enumerator->GetDefaultAudioEndpoint(flow, eConsole, &m_device);
    if (FAILED(hr) || m_device == nullptr)
    {
        error = L"找不到默认";
        error += what;
        error += L"设备 (";
        error += HrText(hr);
        error += L")";
        return false;
    }

    hr = m_device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                            reinterpret_cast<void**>(&m_audioClient));
    if (FAILED(hr))
    {
        error = L"无法激活 ";
        error += what;
        error += L" 的音频客户端 (";
        error += HrText(hr);
        error += L")";
        return false;
    }

    hr = m_audioClient->GetMixFormat(&m_waveFormat);
    if (FAILED(hr) || m_waveFormat == nullptr)
    {
        error = std::wstring(L"无法获取 ") + what + L" 的音频格式";
        return false;
    }

    DWORD flags = loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0;
    const REFERENCE_TIME bufferDuration = 5000000; // 500 ms

    hr = m_audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, bufferDuration, 0,
                                   m_waveFormat, nullptr);
    if (FAILED(hr))
    {
        error = std::wstring(L"无法初始化 ") + what + L" 采集 (";
        error += HrText(hr);
        error += L")";
        return false;
    }

    hr = m_audioClient->GetService(__uuidof(IAudioCaptureClient),
                                  reinterpret_cast<void**>(&m_captureClient));
    if (FAILED(hr) || m_captureClient == nullptr)
    {
        error = std::wstring(L"无法获取 ") + what + L" 的采集服务";
        return false;
    }

    if (!m_writer.Open(m_wavPath, m_waveFormat))
    {
        error = L"无法创建临时文件：";
        error += m_wavPath;
        return false;
    }

    m_storedFormat = m_writer.OutputFormat();
    return true;
}

bool AudioCaptureEngine::Start(std::wstring& error)
{
    if (m_running.load() || m_audioClient == nullptr || m_captureClient == nullptr)
    {
        error = L"采集未就绪";
        return false;
    }

    HRESULT hr = m_audioClient->Start();
    if (FAILED(hr))
    {
        error = L"无法启动音频采集 (";
        error += HrText(hr);
        error += L")";
        return false;
    }

    // Frame 0 of the recorded file corresponds to "now".
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    const LARGE_INTEGER frequency = [] {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return f;
    }();

    m_baseQpc = static_cast<unsigned long long>(
        (counter.QuadPart * 10000000LL) / (frequency.QuadPart ? frequency.QuadPart : 1));
    m_timelineFrames = 0;
    m_framesCaptured = 0;
    m_startTicks = static_cast<UINT64>(counter.QuadPart);
    m_qpcFrequency = static_cast<UINT64>(frequency.QuadPart != 0 ? frequency.QuadPart : 1);
    m_droppedFrames = 0;

    Trace(L"capture start: qpcFrequency=" + std::to_wstring(frequency.QuadPart) +
          L" rawTicks=" + std::to_wstring(counter.QuadPart) + L" baseQpc(100ns)=" +
          std::to_wstring(m_baseQpc) + L" rate=" + std::to_wstring(m_waveFormat->nSamplesPerSec));

    m_running = true;
    m_thread = std::thread(&AudioCaptureEngine::CaptureThread, this);
    return true;
}

void AudioCaptureEngine::Stop()
{
    if (!m_running.exchange(false))
    {
        if (m_thread.joinable())
        {
            m_thread.join();
        }
        m_writer.Close();
        return;
    }
    if (m_audioClient)
    {
        m_audioClient->Stop();
    }

    if (m_thread.joinable())
    {
        m_thread.join();
    }

    // Pad the file so that it always covers the whole recording window, even if
    // the endpoint never delivered a single packet (silent system audio).
    if (m_baseQpc != 0 && m_waveFormat && m_waveFormat->nSamplesPerSec > 0)
    {
        const UINT64 elapsedFrames = WallElapsedFrames();
        if (elapsedFrames > m_timelineFrames)
        {
            Trace(L"tail fill: wall=" + std::to_wstring(elapsedFrames) +
                  L" frames, timeline=" + std::to_wstring(m_timelineFrames) +
                  L" frames, dropped=" + std::to_wstring(m_droppedFrames));
            m_writer.WriteSilenceFrames(elapsedFrames - m_timelineFrames);
            m_timelineFrames = elapsedFrames;
        }
        else if (m_droppedFrames > 0)
        {
            Trace(L"tail: timeline=" + std::to_wstring(m_timelineFrames) +
                  L" frames, wall=" + std::to_wstring(elapsedFrames) +
                  L" frames, dropped=" + std::to_wstring(m_droppedFrames));
        }
    }

    m_writer.Close();
}

UINT64 AudioCaptureEngine::WallElapsedFrames() const
{
    if (m_startTicks == 0 || m_waveFormat == nullptr || m_qpcFrequency == 0)
    {
        return 0;
    }

    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    const UINT64 nowTicks = static_cast<UINT64>(counter.QuadPart);
    if (nowTicks <= m_startTicks)
    {
        return 0;
    }

    return ((nowTicks - m_startTicks) * m_waveFormat->nSamplesPerSec) / m_qpcFrequency;
}

void AudioCaptureEngine::ConsumePacket(BYTE* data, UINT32 frames, DWORD flags, UINT64 qpc)
{
    const UINT32 blockAlign = m_waveFormat->nBlockAlign;
    const UINT32 sampleRate = m_waveFormat->nSamplesPerSec;

    // Keep the timeline continuous: if the packet starts later than expected,
    // the difference was silence.
    if (m_baseQpc != 0 && qpc > m_baseQpc)
    {
        const UINT64 expectedQpc = m_baseQpc + (m_timelineFrames * 10000000ULL) / sampleRate;
        if (qpc > expectedQpc)
        {
            UINT64 gapFrames = ((qpc - expectedQpc) * sampleRate) / 10000000ULL;

            // The recorded timeline can never run further ahead than the real
            // (wall-clock) recording time: a device timestamp that jumps, drifts
            // or uses a different time base must not turn a few seconds of audio
            // into minutes of silence.
            const UINT64 ceiling = WallElapsedFrames() + 2ULL * sampleRate; // 2 s slack
            if (m_timelineFrames + gapFrames > ceiling)
            {
                const UINT64 allowed = (ceiling > m_timelineFrames) ? (ceiling - m_timelineFrames) : 0;
                if (gapFrames > allowed)
                {
                    m_droppedFrames += gapFrames - allowed;

                    static int clampTraceCount = 0;
                    if (clampTraceCount < 10)
                    {
                        ++clampTraceCount;
                        Trace(L"gap clamped: device gap=" + std::to_wstring(gapFrames) +
                              L" frames, allowed=" + std::to_wstring(allowed) +
                              L" frames (timeline=" + std::to_wstring(m_timelineFrames) +
                              L", wall=" + std::to_wstring(ceiling / sampleRate) +
                              L" s, qpc-base=" + std::to_wstring(qpc - m_baseQpc) + L")");
                    }
                }
                gapFrames = allowed;
            }

            if (gapFrames > kMaxGapFrames)
            {
                gapFrames = kMaxGapFrames;
            }

            if (gapFrames > 0)
            {
                m_writer.WriteSilenceFrames(gapFrames);
                m_timelineFrames += gapFrames;
            }
        }
    }

    if (frames > 0)
    {
        if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || data == nullptr)
        {
            m_writer.WriteSilenceFrames(frames);
        }
        else
        {
            m_writer.Write(data, frames * blockAlign);
        }
        m_timelineFrames += frames;
        m_framesCaptured += frames;
    }
}

void AudioCaptureEngine::DrainRemainingPackets()
{
    // IAudioClient::Stop() leaves already captured packets in the buffer; read
    // them so the last few milliseconds of audio are not replaced by silence.
    if (m_captureClient == nullptr || m_waveFormat == nullptr)
    {
        return;
    }

    for (int pass = 0; pass < 64; ++pass)
    {
        UINT32 packetFrames = 0;
        if (FAILED(m_captureClient->GetNextPacketSize(&packetFrames)) || packetFrames == 0)
        {
            break;
        }

        BYTE* data = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        UINT64 qpc = 0;
        if (FAILED(m_captureClient->GetBuffer(&data, &frames, &flags, nullptr, &qpc)))
        {
            break;
        }

        ConsumePacket(data, frames, flags, qpc);
        m_captureClient->ReleaseBuffer(frames);
    }
}

void AudioCaptureEngine::CaptureThread()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    DWORD taskIndex = 0;
    HANDLE avrtHandle = AvSetMmThreadCharacteristicsW(L"Audio", &taskIndex);

    while (m_running.load())
    {
        Sleep(10);
        if (!m_running.load())
        {
            break;
        }

        UINT32 packetFrames = 0;
        if (FAILED(m_captureClient->GetNextPacketSize(&packetFrames)))
        {
            // Typically AUDCLNT_E_DEVICE_INVALIDATED: the endpoint disappeared.
            m_failed = true;
            break;
        }

        while (packetFrames > 0 && m_running.load())
        {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            UINT64 qpc = 0;

            if (FAILED(m_captureClient->GetBuffer(&data, &frames, &flags, nullptr, &qpc)))
            {
                m_failed = true;
                packetFrames = 0;
                break;
            }

            ConsumePacket(data, frames, flags, qpc);
            m_captureClient->ReleaseBuffer(frames);

            if (FAILED(m_captureClient->GetNextPacketSize(&packetFrames)))
            {
                m_failed = true;
                packetFrames = 0;
                break;
            }
        }
    }

    DrainRemainingPackets();

    if (avrtHandle)
    {
        AvRevertMmThreadCharacteristics(avrtHandle);
    }

    CoUninitialize();
}
