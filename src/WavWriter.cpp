#include "WavWriter.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{
bool IsExtensibleFloat(const WAVEFORMATEX* format)
{
    if (format->wFormatTag != WAVE_FORMAT_EXTENSIBLE || format->cbSize < 22)
    {
        return false;
    }
    const WAVEFORMATEXTENSIBLE* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
    return ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
}

bool IsExtensiblePcm(const WAVEFORMATEX* format)
{
    if (format->wFormatTag != WAVE_FORMAT_EXTENSIBLE || format->cbSize < 22)
    {
        return false;
    }
    const WAVEFORMATEXTENSIBLE* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
    return ext->SubFormat == KSDATAFORMAT_SUBTYPE_PCM;
}
} // namespace

WavWriter::~WavWriter()
{
    Close();
}

bool WavWriter::Open(const std::wstring& path, const WAVEFORMATEX* sourceFormat)
{
    if (m_file.is_open() || sourceFormat == nullptr || sourceFormat->nChannels == 0)
    {
        return false;
    }

    const bool isFloat = (sourceFormat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) ||
                         IsExtensibleFloat(sourceFormat);
    const bool isPcm = (sourceFormat->wFormatTag == WAVE_FORMAT_PCM) ||
                       IsExtensiblePcm(sourceFormat);

    m_convert = Convert::None;
    bool storePcm16 = false;

    if (isFloat && sourceFormat->wBitsPerSample == 32)
    {
        m_convert = Convert::Float32ToInt16;
        storePcm16 = true;
    }
    else if (isPcm && sourceFormat->wBitsPerSample == 32)
    {
        m_convert = Convert::Int32ToInt16;
        storePcm16 = true;
    }
    else if (isPcm && sourceFormat->wBitsPerSample == 24)
    {
        m_convert = Convert::Int24ToInt16;
        storePcm16 = true;
    }
    else if (isPcm && sourceFormat->wBitsPerSample == 8)
    {
        m_convert = Convert::Int8ToInt16;
        storePcm16 = true;
    }
    else if (isPcm && sourceFormat->wBitsPerSample == 16)
    {
        storePcm16 = true;
    }

    if (storePcm16)
    {
        m_outFormat.wFormatTag = WAVE_FORMAT_PCM;
        m_outFormat.nChannels = sourceFormat->nChannels;
        m_outFormat.nSamplesPerSec = sourceFormat->nSamplesPerSec;
        m_outFormat.wBitsPerSample = 16;
        m_outFormat.nBlockAlign = static_cast<WORD>(m_outFormat.nChannels * 2);
        m_outFormat.nAvgBytesPerSec = m_outFormat.nSamplesPerSec * m_outFormat.nBlockAlign;
        m_outFormat.cbSize = 0;

        // A plain PCM 'fmt ' chunk is 16 bytes (cbSize excluded), which keeps the
        // file header at the classic 44 bytes.
        m_fmtBlob.assign(sizeof(WAVEFORMATEX) - sizeof(WORD), 0);
        std::memcpy(m_fmtBlob.data(), &m_outFormat, m_fmtBlob.size());
    }
    else
    {
        // Exotic format: keep it as-is.
        const UINT32 fmtSize = sizeof(WAVEFORMATEX) + sourceFormat->cbSize;
        m_outFormat = *sourceFormat;
        m_fmtBlob.assign(fmtSize, 0);
        std::memcpy(m_fmtBlob.data(), sourceFormat, fmtSize);
    }

    m_file.open(path.c_str(), std::ios::binary | std::ios::out | std::ios::trunc);
    if (!m_file.is_open())
    {
        m_fmtBlob.clear();
        return false;
    }

    m_dataBytes = 0;
    m_writeError = false;
    m_peakValid = false;
    m_peak = 0;
    WriteHeader();
    m_dataOffset = static_cast<std::streamoff>(m_file.tellp());
    if (!m_file)
    {
        m_writeError = true;
    }
    return true;
}

void WavWriter::WriteHeader()
{
    const UINT32 riffSize = 0;
    const UINT32 fmtSize = static_cast<UINT32>(m_fmtBlob.size());
    const UINT32 dataSize = 0;

    m_file.write("RIFF", 4);
    m_file.write(reinterpret_cast<const char*>(&riffSize), 4);
    m_file.write("WAVE", 4);
    m_file.write("fmt ", 4);
    m_file.write(reinterpret_cast<const char*>(&fmtSize), 4);
    m_file.write(reinterpret_cast<const char*>(m_fmtBlob.data()), fmtSize);
    m_file.write("data", 4);
    m_file.write(reinterpret_cast<const char*>(&dataSize), 4);
}

void WavWriter::UpdateHeader()
{
    if (!m_file.is_open())
    {
        return;
    }

    const std::streamoff end = static_cast<std::streamoff>(m_file.tellp());

    const UINT32 riffSize = static_cast<UINT32>(end - 8);
    m_file.seekp(4);
    m_file.write(reinterpret_cast<const char*>(&riffSize), 4);

    const UINT32 dataSize = static_cast<UINT32>(m_dataBytes);
    m_file.seekp(m_dataOffset - 4);
    m_file.write(reinterpret_cast<const char*>(&dataSize), 4);

    m_file.seekp(end);
    m_file.flush();
}

UINT32 WavWriter::OutputBytesFor(UINT32 size) const
{
    switch (m_convert)
    {
    case Convert::Float32ToInt16:
    case Convert::Int32ToInt16:
        return (size / 4) * 2;
    case Convert::Int24ToInt16:
        return (size / 3) * 2;
    case Convert::Int8ToInt16:
        return size * 2;
    case Convert::None:
    default:
        return size;
    }
}

void WavWriter::Write(const BYTE* data, UINT32 size)
{
    if (!m_file.is_open() || data == nullptr || size == 0 || m_writeError)
    {
        return;
    }

    const UINT32 outputBytes = OutputBytesFor(size);
    if (m_dataBytes + outputBytes > kMaxDataBytes)
    {
        m_writeError = true;
        return;
    }

    if (m_convert == Convert::None)
    {
        m_file.write(reinterpret_cast<const char*>(data), size);
        m_dataBytes += size;

        // Non-converted data is 16-bit PCM in practice; scan it so that level
        // information stays available for the "is this route silent?" check.
        if (m_outFormat.wBitsPerSample == 16 && size >= sizeof(int16_t))
        {
            m_peakValid = true;
            const int16_t* samples = reinterpret_cast<const int16_t*>(data);
            const size_t count = size / sizeof(int16_t);
            for (size_t index = 0; index < count; ++index)
            {
                const int value = samples[index] < 0 ? -samples[index] : samples[index];
                if (value > static_cast<int>(m_peak))
                {
                    m_peak = static_cast<UINT32>(value);
                }
            }
        }
    }
    else
    {
        AppendConverted(data, size);
    }

    if (!m_file)
    {
        m_writeError = true;
    }
}

void WavWriter::AppendConverted(const BYTE* data, UINT32 size)
{
    // Bytes per input sample depend on the source format; getting this wrong
    // would read past the end of the capture buffer.
    UINT32 bytesPerSample = 0;
    switch (m_convert)
    {
    case Convert::Float32ToInt16:
    case Convert::Int32ToInt16:
        bytesPerSample = 4;
        break;
    case Convert::Int24ToInt16:
        bytesPerSample = 3;
        break;
    case Convert::Int8ToInt16:
        bytesPerSample = 1;
        break;
    case Convert::None:
    default:
        return;
    }

    const UINT32 samples = size / bytesPerSample;
    m_scratch.resize(static_cast<size_t>(samples) * 2);
    int16_t* dst = reinterpret_cast<int16_t*>(m_scratch.data());

    switch (m_convert)
    {
    case Convert::Float32ToInt16:
    {
        const float* src = reinterpret_cast<const float*>(data);
        for (UINT32 i = 0; i < samples; ++i)
        {
            float value = src[i];
            if (!(value == value)) // NaN guard
            {
                value = 0.0f;
            }
            value = std::max(-1.0f, std::min(1.0f, value));
            dst[i] = static_cast<int16_t>(std::lrintf(value * 32767.0f));
        }
        break;
    }
    case Convert::Int32ToInt16:
    {
        const int32_t* src = reinterpret_cast<const int32_t*>(data);
        for (UINT32 i = 0; i < samples; ++i)
        {
            dst[i] = static_cast<int16_t>(src[i] >> 16);
        }
        break;
    }
    case Convert::Int24ToInt16:
    {
        for (UINT32 i = 0; i < samples; ++i)
        {
            const BYTE* p = data + static_cast<size_t>(i) * 3;
            int32_t value = (static_cast<int32_t>(p[2]) << 24) |
                            (static_cast<int32_t>(p[1]) << 16) |
                            (static_cast<int32_t>(p[0]) << 8);
            dst[i] = static_cast<int16_t>(value >> 16);
        }
        break;
    }
    case Convert::Int8ToInt16:
    {
        const BYTE* src = data;
        for (UINT32 i = 0; i < samples; ++i)
        {
            dst[i] = static_cast<int16_t>((static_cast<int>(src[i]) - 128) << 8);
        }
        break;
    }
    default:
        break;
    }

    const UINT32 outBytes = samples * 2;

    // Track the signal level while the samples are already in hand.
    if (samples > 0)
    {
        m_peakValid = true;
        for (UINT32 index = 0; index < samples; ++index)
        {
            const int value = dst[index] < 0 ? -static_cast<int>(dst[index]) : dst[index];
            if (value > static_cast<int>(m_peak))
            {
                m_peak = static_cast<UINT32>(value);
            }
        }
    }

    m_file.write(reinterpret_cast<const char*>(m_scratch.data()), outBytes);
    m_dataBytes += outBytes;
}

void WavWriter::WriteSilenceFrames(UINT64 frames)
{
    if (!m_file.is_open() || frames == 0 || m_outFormat.nBlockAlign == 0 || m_writeError)
    {
        return;
    }

    UINT64 remaining = frames * m_outFormat.nBlockAlign;
    if (m_dataBytes + remaining > kMaxDataBytes)
    {
        // Truncate instead of wrapping the 32-bit RIFF size field.
        m_writeError = true;
        remaining = (m_dataBytes < kMaxDataBytes) ? (kMaxDataBytes - m_dataBytes) : 0;
        remaining -= remaining % m_outFormat.nBlockAlign;
    }

    const size_t chunkBytes = 64 * 1024;
    if (m_silence.empty())
    {
        m_silence.assign(chunkBytes, 0);
    }

    while (remaining > 0)
    {
        const size_t bytes = static_cast<size_t>(std::min<UINT64>(remaining, chunkBytes));
        m_file.write(reinterpret_cast<const char*>(m_silence.data()), bytes);
        m_dataBytes += bytes;
        remaining -= bytes;
    }

    if (!m_file)
    {
        m_writeError = true;
    }
}

void WavWriter::Close()
{
    if (!m_file.is_open())
    {
        return;
    }

    UpdateHeader();
    if (!m_file)
    {
        m_writeError = true;
    }
    m_file.close();
    m_scratch.clear();
    m_silence.clear();
}
