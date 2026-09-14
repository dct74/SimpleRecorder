#pragma once

#include <windows.h>
#include <mfplay.h>

#include <string>

// Thin wrapper around the Windows Media Foundation media player (MFPlay),
// used for the preview/playback button.
class MediaPlayer : public IMFPMediaPlayerCallback
{
public:
    explicit MediaPlayer(HWND notifyWindow);
    ~MediaPlayer();

    MediaPlayer(const MediaPlayer&) = delete;
    MediaPlayer& operator=(const MediaPlayer&) = delete;

    bool PlayFile(const std::wstring& path, std::wstring& error);
    void Stop();
    void Shutdown();

    bool IsPlaying() const { return m_active; }
    bool TakeError() { const bool value = m_error; m_error = false; return value; }

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** object) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // IMFPMediaPlayerCallback
    STDMETHODIMP_(void) OnMediaPlayerEvent(MFP_EVENT_HEADER* eventHeader) override;

private:
    HWND m_notifyWindow = nullptr;
    IMFPMediaPlayer* m_player = nullptr;
    IMFPMediaItem* m_item = nullptr;
    LONG m_refCount = 1;
    bool m_active = false;
    bool m_error = false;
    bool m_shutdown = false;
};
