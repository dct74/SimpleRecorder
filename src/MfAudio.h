#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace mfaudio
{
bool Startup();
void Shutdown();

struct InputFile
{
    std::wstring path;
    // QPC timestamp (100 ns units) of frame 0 of the file; 0 when unknown.
    unsigned long long startQpc = 0;
};

struct MixResult
{
    UINT64 durationMs = 0;
    UINT64 inputFrames = 0;
    UINT32 sampleRate = 0;
    UINT32 channels = 0;
};

// Turns one or more PCM WAV files into a single .m4a file using the Windows
// Media Foundation pipeline only:
//
//   * IMFSourceReader          - decode the temporary WAV files
//   * Audio Resampler DSP      - native sample-rate / channel conversion
//   * (summation in-process)   - frame aligned addition of the routes
//   * AAC encoder MFT          - native AAC-LC encoding
//   * MPEG-4 media sink        - native .m4a container writer
//
// The routes are aligned through their 'startQpc' values, so the two files do
// not have to be started at exactly the same instant.
bool MixToM4a(const std::vector<InputFile>& inputs,
              const std::wstring& outPath,
              UINT32 bitrateBps,
              std::wstring& error,
              MixResult* result = nullptr);
} // namespace mfaudio
