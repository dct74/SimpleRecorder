#include "MfAudio.h"

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>

namespace mfaudio
{
namespace
{
std::wstring HrText(HRESULT hr)
{
    wchar_t buffer[64];
    swprintf_s(buffer, L"0x%08X", static_cast<unsigned int>(hr));
    return buffer;
}

UINT32 GetUint32(IMFMediaType* type, REFGUID key, UINT32 fallback)
{
    UINT32 value = 0;
    if (type != nullptr && SUCCEEDED(type->GetUINT32(key, &value)))
    {
        return value;
    }
    return fallback;
}

// A pseudo (node-less) fault helper: builds "msg (0x...)".
//---------------------------------------------------------------------------// Optional diagnostic trace: set the SIMPLERECORDER_TRACE environment variable
// to a file path to log every stage of the conversion/mix job.
//---------------------------------------------------------------------------
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

std::wstring WithHr(const wchar_t* message, HRESULT hr)
{
    std::wstring text(message);
    text += L" (";
    text += HrText(hr);
    text += L")";
    return text;
}

bool IsSupportedAacRate(UINT32 rate)
{
    switch (rate)
    {
    case 8000:
    case 11025:
    case 12000:
    case 16000:
    case 22050:
    case 24000:
    case 32000:
    case 44100:
    case 48000:
        return true;
    default:
        return false;
    }
}

const UINT32 kChunkFrames = 1024;

// One decoded route, kept in the canonical PCM format of the mix job.
struct Track
{
    IMFSourceReader* reader = nullptr;
    bool endOfStream = false;
    std::vector<BYTE> pending; // decoded PCM, not yet consumed
    size_t cursor = 0;
    UINT64 padFrames = 0; // leading silence (alignment offset)
    UINT32 blockAlign = 4;

    ~Track()
    {
        if (reader != nullptr)
        {
            reader->Release();
        }
    }

    UINT32 PendingFrames() const
    {
        return blockAlign == 0 ? 0 : static_cast<UINT32>((pending.size() - cursor) / blockAlign);
    }
};

void Compact(Track& track)
{
    if (track.cursor == 0)
    {
        return;
    }
    if (track.cursor >= track.pending.size())
    {
        track.pending.clear();
        track.cursor = 0;
    }
    else if (track.cursor > 1u << 20)
    {
        track.pending.erase(track.pending.begin(),
                            track.pending.begin() + static_cast<std::ptrdiff_t>(track.cursor));
        track.cursor = 0;
    }
}

// Reads until at least 'frames' frames are buffered (or the stream ends).
bool EnsurePending(Track& track, UINT32 frames)
{
    Compact(track);

    int emptyReads = 0;
    while (!track.endOfStream && track.PendingFrames() < frames)
    {
        DWORD flags = 0;
        IMFSample* sample = nullptr;
        const HRESULT hr = track.reader->ReadSample(
            static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0, nullptr, &flags, nullptr, &sample);
        if (FAILED(hr))
        {
            if (sample != nullptr)
            {
                sample->Release();
            }
            return false;
        }

        if (sample == nullptr)
        {
            if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0 || ++emptyReads > 100)
            {
                track.endOfStream = true;
            }
            continue;
        }

        DWORD copied = 0;
        IMFMediaBuffer* buffer = nullptr;
        if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buffer)) && buffer != nullptr)
        {
            BYTE* data = nullptr;
            DWORD length = 0;
            if (SUCCEEDED(buffer->Lock(&data, nullptr, &length)) && data != nullptr && length > 0)
            {
                track.pending.insert(track.pending.end(), data, data + length);
                copied = length;
                buffer->Unlock();
            }
            buffer->Release();
        }
        sample->Release();

        // Guard against a stream that keeps handing out empty samples.
        if (copied == 0)
        {
            if (++emptyReads > 100)
            {
                track.endOfStream = true;
            }
        }
        else
        {
            emptyReads = 0;
        }

        if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0)
        {
            track.endOfStream = true;
        }
    }

    return true;
}

// Copies 'frames' frames out of the track as 16-bit PCM.  Alignment padding and
// everything past the end of the stream yield silence.
void ReadTrackFrames(Track& track, UINT32 frames, int16_t* destination, UINT32 channels)
{
    UINT32 frame = 0;

    while (frame < frames && track.padFrames > 0)
    {
        for (UINT32 channel = 0; channel < channels; ++channel)
        {
            destination[static_cast<size_t>(frame) * channels + channel] = 0;
        }
        --track.padFrames;
        ++frame;
    }

    const UINT32 buffered = track.PendingFrames();
    const UINT32 fromBuffer = std::min(frames - frame, buffered);

    if (fromBuffer > 0)
    {
        const int16_t* source =
            reinterpret_cast<const int16_t*>(track.pending.data() + track.cursor);
        for (UINT32 index = 0; index < fromBuffer; ++index)
        {
            for (UINT32 channel = 0; channel < channels; ++channel)
            {
                destination[static_cast<size_t>(frame + index) * channels + channel] =
                    source[static_cast<size_t>(index) * channels + channel];
            }
        }
        track.cursor += static_cast<size_t>(fromBuffer) * track.blockAlign;
        frame += fromBuffer;
    }

    for (; frame < frames; ++frame)
    {
        for (UINT32 channel = 0; channel < channels; ++channel)
        {
            destination[static_cast<size_t>(frame) * channels + channel] = 0;
        }
    }
}

HRESULT CreateSinkWriter(const std::wstring& path,
                         IMFMediaType* outputType,
                         IMFMediaSink** sinkOut,
                         IMFFinalizableMediaSink** finalizableOut)
{
    *sinkOut = nullptr;
    *finalizableOut = nullptr;

    IMFByteStream* stream = nullptr;
    HRESULT hr = MFCreateFile(MF_ACCESSMODE_READWRITE, MF_OPENMODE_DELETE_IF_EXIST,
                              MF_FILEFLAGS_NONE, path.c_str(), &stream);
    if (FAILED(hr))
    {
        return hr;
    }

    IMFMediaSink* created = nullptr;
    hr = MFCreateMPEG4MediaSink(stream, nullptr, outputType, &created);
    stream->Release();
    if (FAILED(hr))
    {
        return hr;
    }

    IMFFinalizableMediaSink* mpeg4 = nullptr;
    hr = created->QueryInterface(IID_PPV_ARGS(&mpeg4));
    if (FAILED(hr))
    {
        created->Release();
        return hr;
    }

    *sinkOut = created; // reference transferred
    *finalizableOut = mpeg4;
    return S_OK;
}

HRESULT OpenReader(const std::wstring& path, IMFSourceReader** readerOut,
                   UINT32* sampleRate, UINT32* channels)
{
    *readerOut = nullptr;

    IMFAttributes* attributes = nullptr;
    HRESULT hr = MFCreateAttributes(&attributes, 2);
    if (FAILED(hr))
    {
        return hr;
    }

    // When the requested output type differs from the native type, the source
    // reader inserts the native Audio Resampler DSP on its own, which performs
    // the sample-rate conversion and channel mixing for us.
    attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, FALSE);

    hr = MFCreateSourceReaderFromURL(path.c_str(), attributes, readerOut);
    attributes->Release();
    if (FAILED(hr))
    {
        return hr;
    }

    (*readerOut)->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    const HRESULT selectHr = (*readerOut)->SetStreamSelection(
        static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), TRUE);

    IMFMediaType* native = nullptr;
    hr = (*readerOut)->GetNativeMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM),
                                          0, &native);
    if (SUCCEEDED(hr) && native != nullptr)
    {
        *sampleRate = GetUint32(native, MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
        *channels = GetUint32(native, MF_MT_AUDIO_NUM_CHANNELS, 2);
        native->Release();
    }
    else
    {
        *sampleRate = 48000;
        *channels = 2;
    }

    return selectHr;
}
} // namespace

bool Startup()
{
    return SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_FULL));
}

void Shutdown()
{
    MFShutdown();
}

bool MixToM4a(const std::vector<InputFile>& inputs,
              const std::wstring& outPath,
              UINT32 bitrateBps,
              std::wstring& error,
              MixResult* result)
{
    error.clear();
    Trace(L"--- MixToM4a start, inputs=" + std::to_wstring(inputs.size()));

    if (inputs.empty())
    {
        error = L"没有可用的录音数据。";
        return false;
    }

    // ---------------------------------------------------------------- readers
    std::vector<std::unique_ptr<Track>> tracks;
    UINT32 firstRate = 48000;
    UINT32 firstChannels = 2;

    for (size_t index = 0; index < inputs.size(); ++index)
    {
        auto track = std::make_unique<Track>();

        UINT32 rate = 48000;
        UINT32 channels = 2;
        const HRESULT hr = OpenReader(inputs[index].path, &track->reader, &rate, &channels);
        if (FAILED(hr) || track->reader == nullptr)
        {
            error = L"无法打开临时音频文件：\n";
            error += inputs[index].path;
            error += L"\n";
            error += WithHr(L"错误", hr);
            return false;
        }

        Trace(L"reader " + std::to_wstring(index) + L" opened, native " +
              std::to_wstring(rate) + L" Hz / " + std::to_wstring(channels) + L" ch");

        if (index == 0)
        {
            firstRate = rate;
            firstChannels = channels;
        }

        tracks.push_back(std::move(track));
    }

    // ------------------------------------------------------- canonical format
    UINT32 channels = 2;
    UINT32 sampleRate = 48000;

    if (tracks.size() == 1)
    {
        channels = std::min<UINT32>(std::max<UINT32>(firstChannels, 1), 2);
        sampleRate = IsSupportedAacRate(firstRate) ? firstRate : 48000;
    }

    const UINT32 blockAlign = channels * 2;

    for (size_t index = 0; index < tracks.size(); ++index)
    {
        IMFMediaType* outputType = nullptr;
        HRESULT hr = MFCreateMediaType(&outputType);
        if (FAILED(hr))
        {
            error = WithHr(L"无法创建音频类型", hr);
            return false;
        }

        outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        outputType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        outputType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
        outputType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sampleRate);
        outputType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        outputType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, blockAlign);
        outputType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, sampleRate * blockAlign);

        hr = tracks[index]->reader->SetCurrentMediaType(
            static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), nullptr, outputType);
        outputType->Release();
        if (FAILED(hr))
        {
            error = WithHr(L"无法把临时音频转换到统一的 PCM 格式", hr);
            return false;
        }

        tracks[index]->blockAlign = blockAlign;
    }
    Trace(L"canonical format " + std::to_wstring(sampleRate) + L" Hz / " +
          std::to_wstring(channels) + L" ch");

    // ------------------------------------------------------------- alignment
    unsigned long long baseQpc = 0;
    for (const InputFile& input : inputs)
    {
        if (input.startQpc != 0)
        {
            baseQpc = (baseQpc == 0) ? input.startQpc : std::min(baseQpc, input.startQpc);
        }
    }

    if (baseQpc != 0 && tracks.size() > 1)
    {
        for (size_t index = 0; index < tracks.size(); ++index)
        {
            if (inputs[index].startQpc > baseQpc)
            {
                UINT64 pad = ((inputs[index].startQpc - baseQpc) * sampleRate) / 10000000ULL;
                pad = std::min<UINT64>(pad, static_cast<UINT64>(sampleRate) * 5); // sanity: 5 s
                tracks[index]->padFrames = pad;
            }
        }
    }

    // ----------------------------------------------------------- sink writer
    IMFMediaType* aacType = nullptr;
    IMFMediaType* pcmType = nullptr;
    IMFAttributes* writerAttributes = nullptr;
    IMFMediaSink* sink = nullptr;
    IMFFinalizableMediaSink* finalizable = nullptr;
    IMFSinkWriter* writer = nullptr;
    DWORD streamIndex = 0;

    // Local cleanup helper.
    auto releaseAll = [&]() {
        if (writer != nullptr)
        {
            writer->Release();
            writer = nullptr;
        }
        if (finalizable != nullptr)
        {
            finalizable->Release();
            finalizable = nullptr;
        }
        if (sink != nullptr)
        {
            sink->Release();
            sink = nullptr;
        }
        if (writerAttributes != nullptr)
        {
            writerAttributes->Release();
            writerAttributes = nullptr;
        }
        if (pcmType != nullptr)
        {
            pcmType->Release();
            pcmType = nullptr;
        }
        if (aacType != nullptr)
        {
            aacType->Release();
            aacType = nullptr;
        }
    };

    HRESULT hr = MFCreateMediaType(&aacType);
    Trace(L"building AAC/MPEG-4 output type and PCM input type");
    if (SUCCEEDED(hr))
    {
        aacType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        aacType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
        aacType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        aacType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
        aacType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sampleRate);
        aacType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, bitrateBps / 8);
        aacType->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);              // raw AAC, required by MPEG-4
        aacType->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29); // AAC-LC
    }

    if (SUCCEEDED(hr))
    {
        hr = MFCreateMediaType(&pcmType);
    }
    if (SUCCEEDED(hr))
    {
        pcmType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        pcmType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        pcmType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
        pcmType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sampleRate);
        pcmType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        pcmType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, blockAlign);
        pcmType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, sampleRate * blockAlign);
    }

    if (SUCCEEDED(hr))
    {
        hr = CreateSinkWriter(outPath, aacType, &sink, &finalizable);
        Trace(L"CreateSinkWriter hr=" + HrText(hr));
    }

    if (SUCCEEDED(hr))
    {
        hr = MFCreateAttributes(&writerAttributes, 2);
    }
    if (SUCCEEDED(hr))
    {
        writerAttributes->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
        hr = MFCreateSinkWriterFromMediaSink(sink, writerAttributes, &writer);
        Trace(L"CreateSinkWriterFromMediaSink hr=" + HrText(hr));
    }

    if (SUCCEEDED(hr))
    {
        hr = writer->AddStream(aacType, &streamIndex);
        Trace(L"AddStream hr=" + HrText(hr));
    }
    if (SUCCEEDED(hr))
    {
        hr = writer->SetInputMediaType(streamIndex, pcmType, nullptr);
        Trace(L"SetInputMediaType hr=" + HrText(hr));
    }
    if (SUCCEEDED(hr))
    {
        hr = writer->BeginWriting();
        Trace(L"BeginWriting hr=" + HrText(hr));
    }

    if (FAILED(hr))
    {
        error = L"无法初始化 M4A (AAC) 编码管线：\n";
        error += WithHr(L"错误", hr);
        error += L"\n（若系统缺少媒体功能，请安装 Windows Media Feature Pack）";
        releaseAll();
        return false;
    }

    // -------------------------------------------------------------- mixing
    std::vector<int16_t> accumulator(static_cast<size_t>(kChunkFrames) * channels);
    std::vector<int16_t> scratch(static_cast<size_t>(kChunkFrames) * channels);
    std::vector<BYTE> mixBuffer(static_cast<size_t>(kChunkFrames) * blockAlign);

    UINT64 framesWritten = 0;
    bool ok = true;

    for (;;)
    {
        bool anyWork = false;

        for (auto& track : tracks)
        {
            if (!track->endOfStream)
            {
                if (!EnsurePending(*track, kChunkFrames))
                {
                    ok = false;
                    break;
                }
            }
            if (!track->endOfStream || track->PendingFrames() > 0 || track->padFrames > 0)
            {
                anyWork = true;
            }
        }

        if (!ok || !anyWork)
        {
            break;
        }

        UINT32 frames = kChunkFrames;
        for (auto& track : tracks)
        {
            const UINT32 available = track->endOfStream
                ? kChunkFrames
                : static_cast<UINT32>(std::min<UINT64>(
                      static_cast<UINT64>(track->padFrames) + track->PendingFrames(), kChunkFrames));
            frames = std::min(frames, available);
        }

        if (frames == 0)
        {
            break;
        }

        const size_t sampleCount = static_cast<size_t>(frames) * channels;
        std::memset(accumulator.data(), 0, sampleCount * sizeof(int16_t));

        for (auto& track : tracks)
        {
            ReadTrackFrames(*track, frames, scratch.data(), channels);
            for (size_t i = 0; i < sampleCount; ++i)
            {
                accumulator[i] = static_cast<int16_t>(
                    std::max(-32768, std::min(32767, accumulator[i] + scratch[i])));
            }
        }

        std::memcpy(mixBuffer.data(), accumulator.data(), sampleCount * sizeof(int16_t));

        IMFMediaBuffer* mediaBuffer = nullptr;
        hr = MFCreateMemoryBuffer(static_cast<DWORD>(frames * blockAlign), &mediaBuffer);
        if (FAILED(hr))
        {
            ok = false;
            break;
        }

        BYTE* destination = nullptr;
        hr = mediaBuffer->Lock(&destination, nullptr, nullptr);
        if (SUCCEEDED(hr) && destination != nullptr)
        {
            std::memcpy(destination, mixBuffer.data(), static_cast<size_t>(frames) * blockAlign);
            mediaBuffer->Unlock();
            mediaBuffer->SetCurrentLength(frames * blockAlign);
        }

        IMFSample* sample = nullptr;
        hr = MFCreateSample(&sample);
        if (SUCCEEDED(hr))
        {
            sample->AddBuffer(mediaBuffer);
            sample->SetSampleTime(static_cast<LONGLONG>(framesWritten) * 10000000LL / sampleRate);
            sample->SetSampleDuration(static_cast<LONGLONG>(frames) * 10000000LL / sampleRate);
            hr = writer->WriteSample(streamIndex, sample);
            sample->Release();
        }

        mediaBuffer->Release();

        if (FAILED(hr))
        {
            ok = false;
            break;
        }

        framesWritten += frames;

        static int traceCounter = 0;
        if (++traceCounter % 200 == 0)
        {
            Trace(L"mixed frames=" + std::to_wstring(framesWritten));
        }
    }

    Trace(L"loop finished, frames=" + std::to_wstring(framesWritten));

    HRESULT finalizeHr = S_OK;
    if (ok)
    {
        finalizeHr = writer->Finalize();
        Trace(L"Finalize hr=" + HrText(finalizeHr));
    }

    releaseAll();

    if (!ok)
    {
        error = L"合成过程中读取音频数据失败。";
        DeleteFileW(outPath.c_str());
        return false;
    }

    if (FAILED(finalizeHr))
    {
        error = L"无法完成 M4A 文件写入：\n";
        error += WithHr(L"错误", finalizeHr);
        DeleteFileW(outPath.c_str());
        return false;
    }

    if (framesWritten == 0)
    {
        error = L"录音内容为空，未生成文件。";
        DeleteFileW(outPath.c_str());
        return false;
    }

    if (result != nullptr)
    {
        result->inputFrames = framesWritten;
        result->sampleRate = sampleRate;
        result->channels = channels;
        result->durationMs = framesWritten * 1000ULL / sampleRate;
    }

    return true;
}
} // namespace mfaudio
