#include "MediaPlayer.h"

#include <mfapi.h>

#include "AppMessages.h"

MediaPlayer::MediaPlayer(HWND notifyWindow)
    : m_notifyWindow(notifyWindow)
{
}

MediaPlayer::~MediaPlayer()
{
    Shutdown();
}

STDMETHODIMP MediaPlayer::QueryInterface(REFIID riid, void** object)
{
    if (object == nullptr)
    {
        return E_POINTER;
    }

    if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFPMediaPlayerCallback))
    {
        *object = static_cast<IMFPMediaPlayerCallback*>(this);
        AddRef();
        return S_OK;
    }

    *object = nullptr;
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) MediaPlayer::AddRef()
{
    return static_cast<ULONG>(InterlockedIncrement(&m_refCount));
}

STDMETHODIMP_(ULONG) MediaPlayer::Release()
{
    // The player is owned by the application, never by MFPlay.
    return static_cast<ULONG>(InterlockedDecrement(&m_refCount));
}

STDMETHODIMP_(void) MediaPlayer::OnMediaPlayerEvent(MFP_EVENT_HEADER* eventHeader)
{
    if (eventHeader == nullptr || m_shutdown)
    {
        return;
    }

    switch (eventHeader->eEventType)
    {
    case MFP_EVENT_TYPE_PLAYBACK_ENDED:
        m_active = false;
        PostMessageW(m_notifyWindow, WM_APP_PLAYBACK_ENDED, 0, 0);
        break;

    case MFP_EVENT_TYPE_ERROR:
        m_error = true;
        m_active = false;
        PostMessageW(m_notifyWindow, WM_APP_PLAYBACK_ENDED, 0, 0);
        break;

    default:
        break;
    }
}

bool MediaPlayer::PlayFile(const std::wstring& path, std::wstring& error)
{
    error.clear();

    if (m_shutdown)
    {
        error = L"播放器已关闭。";
        return false;
    }

    if (m_player == nullptr)
    {
        const HRESULT hr = MFPCreateMediaPlayer(nullptr, FALSE, MFP_OPTION_NONE, this,
                                                m_notifyWindow, &m_player);
        if (FAILED(hr) || m_player == nullptr)
        {
            wchar_t buffer[128];
            swprintf_s(buffer, L"无法创建系统媒体播放器 (0x%08X)。",
                       static_cast<unsigned int>(hr));
            error = buffer;
            return false;
        }
        m_player->SetVolume(1.0f);
    }

    if (m_item != nullptr)
    {
        m_player->ClearMediaItem();
        m_item->Release();
        m_item = nullptr;
    }

    // Synchronous item creation: the returned item is ready to be set.
    HRESULT hr = m_player->CreateMediaItemFromURL(path.c_str(), TRUE, 0, &m_item);
    if (FAILED(hr) || m_item == nullptr)
    {
        wchar_t buffer[160];
        swprintf_s(buffer, L"无法打开要回放的录音文件 (0x%08X)。",
                   static_cast<unsigned int>(hr));
        error = buffer;
        return false;
    }

    hr = m_player->SetMediaItem(m_item);
    if (SUCCEEDED(hr))
    {
        hr = m_player->Play();
    }

    if (FAILED(hr))
    {
        wchar_t buffer[160];
        swprintf_s(buffer, L"回放失败 (0x%08X)。", static_cast<unsigned int>(hr));
        error = buffer;
        return false;
    }

    m_active = true;
    return true;
}

void MediaPlayer::Stop()
{
    if (m_player == nullptr)
    {
        return;
    }

    if (m_active)
    {
        m_player->Stop();
    }
    m_active = false;

    if (m_item != nullptr)
    {
        m_player->ClearMediaItem();
        m_item->Release();
        m_item = nullptr;
    }
}

void MediaPlayer::Shutdown()
{
    if (m_shutdown)
    {
        return;
    }
    m_shutdown = true;

    if (m_player != nullptr)
    {
        m_player->Stop();
        if (m_item != nullptr)
        {
            m_player->ClearMediaItem();
            m_item->Release();
            m_item = nullptr;
        }
        m_player->Shutdown();
        m_player->Release();
        m_player = nullptr;
    }
    else if (m_item != nullptr)
    {
        m_item->Release();
        m_item = nullptr;
    }

    m_active = false;
}
