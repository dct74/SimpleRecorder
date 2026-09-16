#pragma once

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>

#include <atomic>
#include <string>
#include <thread>

#include "WavWriter.h"

enum class CaptureRoute
{
    SystemLoopback, // default render endpoint, WASAPI loopback
    Microphone      // default capture endpoint
};

// A single WASAPI capture route.
//
// The engine writes a continuous 16-bit PCM WAV timeline:
//   * the timeline origin is the QPC timestamp taken right after IAudioClient::Start()
//   * gaps in the packet stream (loopback endpoints deliver nothing while the
//     machine is silent) are filled with silence, so that two independent routes
//     stay sample-aligned with each other
class AudioCaptureEngine
{
public:
    AudioCaptureEngine() = default;
    ~AudioCaptureEngine();

    AudioCaptureEngine(const AudioCaptureEngine&) = delete;
    AudioCaptureEngine& operator=(const AudioCaptureEngine&) = delete;

    bool Open(CaptureRoute route, const std::wstring& tempWavPath, std::wstring& error);
    bool Start(std::wstring& error);
    void Stop();

    // Releases every COM interface.  Must be called before CoUninitialize() of the
    // owning thread; also done by the destructor.  Safe to call more than once.
    void Close();

    bool CaptureFailed() const { return m_failed.load(); }
    bool WriteFailed() const { return m_writer.HasWriteError(); }
    bool HasProblem() const { return CaptureFailed() || WriteFailed(); }

    // Frames of "silence" that were refused because the device timestamp was not
    // plausible (the recorded timeline may never run ahead of the wall clock).
    UINT64 DroppedFrames() const { return m_droppedFrames; }

    // Measured clock rate of this route in samples per second (0 = not measured).
    // Two routes clocked by different devices drift apart by a few ppm, which the
    // mixer compensates with this value.  Valid after Stop().
    double MeasuredSampleRate() const { return m_measuredRate; }

    // Signal level of this route; a valid peak of 0 means the whole route was
    // digital silence (muted device, wrong device, or nothing playing).
    bool PeakLevelValid() const { return m_writer.PeakLevelValid(); }
    UINT32 PeakLevel() const { return m_writer.PeakLevel(); }
    const WAVEFORMATEX* CaptureFormat() const { return m_waveFormat; }
    const WAVEFORMATEX* StoredFormat() const { return &m_storedFormat; }
    UINT64 FramesCaptured() const { return m_framesCaptured.load(); }
    const std::wstring& TempWavPath() const { return m_wavPath; }

    // QPC value (100 ns units) that corresponds to frame 0 of the stored file.
    // Used to align the two routes when mixing.
    unsigned long long BaseQpc() const { return m_baseQpc; }

    std::wstring DescribeFormat() const;

private:
    void CaptureThread();
    // Writes one captured packet (inserting any gap silence) to the temp file.
    void ConsumePacket(BYTE* data, UINT32 frames, DWORD flags, UINT64 qpc);
    // Pulls the packets that are still buffered after the stream was stopped.
    void DrainRemainingPackets();
    // Frames the real (wall-clock) recording time amounts to, measured with our
    // own counter.  Used as the upper bound for any silence we insert, so that a
    // bogus device timestamp can never inflate the file.
    UINT64 WallElapsedFrames() const;

    IMMDeviceEnumerator* m_enumerator = nullptr;
    IMMDevice* m_device = nullptr;
    IAudioClient* m_audioClient = nullptr;
    IAudioCaptureClient* m_captureClient = nullptr;
    WAVEFORMATEX* m_waveFormat = nullptr;

    WavWriter m_writer;
    WAVEFORMATEX m_storedFormat{};
    std::wstring m_wavPath;

    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_failed{false};
    std::atomic<UINT64> m_framesCaptured{0};

    unsigned long long m_baseQpc = 0;
    UINT64 m_timelineFrames = 0; // frames accounted for (captured data + inserted silence)
    UINT64 m_startTicks = 0;      // our own counter when the stream was started
    UINT64 m_qpcFrequency = 1;    // QueryPerformanceFrequency at start
    UINT64 m_droppedFrames = 0;   // silence refused because it was not plausible

    // Clock probe: compares how many frames a route delivered with the device
    // time stamps that carried them.  Pairs separated by a gap we synthesised are
    // skipped, because invented silence carries no clock information.
    UINT64 m_probeFrames = 0;     // audio frames across contiguous packet pairs
    UINT64 m_probeQpc = 0;        // device time stamps spanned by those frames
    UINT64 m_probePrevQpc = 0;    // device time stamp of the previous packet
    UINT64 m_probePrevFrames = 0; // captured frames before that packet
    double m_measuredRate = 0.0;  // samples per second, filled in by Stop()
};
