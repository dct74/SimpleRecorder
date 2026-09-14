#pragma once

#include <windows.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#include <string>
#include <vector>
#include <fstream>

// Minimal WAV writer, used for the temporary capture files.
//
// The capture path always stores 16-bit PCM to keep the temporary files small
// (the Windows shared-mode capture format is almost always 32-bit float, which
// would otherwise cost ~1.3 GB per hour).  Formats that cannot be converted are
// written verbatim.
class WavWriter
{
public:
    WavWriter() = default;
    ~WavWriter();

    WavWriter(const WavWriter&) = delete;
    WavWriter& operator=(const WavWriter&) = delete;

    // Opens 'path' and prepares the output format derived from 'sourceFormat'.
    bool Open(const std::wstring& path, const WAVEFORMATEX* sourceFormat);

    // Appends captured data (already in 'sourceFormat').
    void Write(const BYTE* data, UINT32 size);

    // Appends silence, used to keep the recorded timeline continuous.
    void WriteSilenceFrames(UINT64 frames);

    void Close();

    // True when the file could not be written (disk full, ...) or when the 4 GB
    // RIFF limit was reached.  Checked by the UI after the capture has stopped.
    bool HasWriteError() const { return m_writeError; }

    // Highest absolute 16-bit sample written so far.  Valid only when
    // PeakLevelValid() is true; a peak of 0 means the route recorded digital
    // silence for its whole duration.
    bool PeakLevelValid() const { return m_peakValid; }
    UINT32 PeakLevel() const { return m_peak; }

    // Format actually stored in the file (always 16-bit PCM unless the source
    // format could not be converted).
    const WAVEFORMATEX& OutputFormat() const { return m_outFormat; }

private:
    enum class Convert
    {
        None,
        Float32ToInt16,
        Int32ToInt16,
        Int24ToInt16,
        Int8ToInt16
    };

    void WriteHeader();
    void UpdateHeader();
    void AppendConverted(const BYTE* data, UINT32 size);

    // Bytes that 'size' input bytes will occupy in the stored format.
    UINT32 OutputBytesFor(UINT32 size) const;

    // A RIFF/WAVE file stores its sizes in 32-bit fields; stop before wrapping.
    static constexpr UINT64 kMaxDataBytes = 3900000000ULL;

    std::ofstream m_file;
    std::vector<BYTE> m_fmtBlob;   // 'fmt ' chunk payload
    WAVEFORMATEX m_outFormat{};
    Convert m_convert = Convert::None;
    std::vector<BYTE> m_scratch;   // conversion scratch (audio samples)
    std::vector<BYTE> m_silence;   // zero-filled block for silence padding
    UINT64 m_dataBytes = 0;
    std::streamoff m_dataOffset = 0;
    bool m_writeError = false;
    bool m_peakValid = false;
    UINT32 m_peak = 0;
};
