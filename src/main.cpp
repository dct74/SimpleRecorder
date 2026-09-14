// SimpleRecorder - a minimal native Windows recorder.
//
//   * record system audio only (WASAPI loopback of the default render endpoint)
//   * record the microphone only (default capture endpoint)
//   * record both as two independent routes and merge them into one file after
//     the recording has stopped, using the native Media Foundation pipeline
//
// All output is written as .m4a (AAC in an MPEG-4 container).

#include <windows.h>
#include <commctrl.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlobj_core.h>
#include <shobjidl.h>
#include <shlwapi.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "AppMessages.h"
#include "AudioCaptureEngine.h"
#include "MediaPlayer.h"
#include "MfAudio.h"

namespace
{
// Optional diagnostic trace: set SIMPLERECORDER_TRACE to a file path.
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

const wchar_t* const kWindowClass = L"SimpleRecorderMainWindow";
const wchar_t* const kWindowTitle = L"录音工具 — 系统声音 / 麦克风 / 混合（M4A）";
const wchar_t* const kTempFolderName = L"SimpleRecorder";
const UINT32 kAacBitrate = 192000; // bits per second

const int kMinimumClientWidth = 460; // logical (96 DPI) units, grown if needed
const int kExtraStatusHeight = 36;   // extra breathing room for the status box

const wchar_t* const kHintText =
    L"输出格式：M4A（AAC，由 Windows 原生 Media Foundation 管线编码）。\n"
    L"混合录制会同时录下两路（系统声音与麦克风各一路），停止后对齐并合成为一个文件。";

const wchar_t* const kModeLabels[3] = {
    L"① 只录制系统声音（电脑正在播放的声音）",
    L"② 只录制麦克风声音",
    L"③ 混合录制（系统声音 + 麦克风，停止后合成为一个文件）"};

// Longest text the status box can ever show (used to derive the minimum height).
const wchar_t* const kWorstCaseStatusText =
    L"✔ 录音完成：混合录制（系统声音 + 麦克风）（尚未保存）\n"
    L"时长 00:00:00，文件大小 999.99 MB\n"
    L"临时文件：%TEMP%\\SimpleRecorder\\rec-20260914-235959-999\\result.m4a（退出时删除）\n"
    L"⚠ 录音设备返回了异常时间戳，已按真实录制时长忽略 999.9 秒静音\n"
    L"⚠ 麦克风轨全程没有声音（检查是否被静音／选错设备／静音键）\n"
    L"⚠ 系统声音轨全程没有声音（录制时没有任何程序在播放？）\n"
    L"可回放试听、另存为 M4A，或直接点“开始录制”录下一段。";

HINSTANCE g_instance = nullptr;
HWND g_window = nullptr;

HWND g_groupMode = nullptr;
HWND g_modeRadios[3] = {nullptr, nullptr, nullptr};
HWND g_recordButton = nullptr;
HWND g_playButton = nullptr;
HWND g_saveButton = nullptr;
HWND g_statusText = nullptr;
HWND g_hintText = nullptr;

HFONT g_font = nullptr;
HBRUSH g_backgroundBrush = nullptr;
UINT g_dpi = 96;
int g_lineHeight = 0; // measured from the UI font, 0 = not measured yet
int g_hintHeight = 0;

enum class State
{
    Idle,
    Recording,
    Processing,
    Ready
};

State g_state = State::Idle;

std::unique_ptr<AudioCaptureEngine> g_systemEngine;
std::unique_ptr<AudioCaptureEngine> g_micEngine;

std::wstring g_sessionDir;
std::wstring g_resultPath;
std::wstring g_captureNote; // shown in the status when the device timestamps were odd
ULONGLONG g_recordStartTick = 0;
UINT64 g_resultDurationMs = 0;
int g_lastMode = 2;
bool g_resultSaved = false;

std::thread g_worker;
std::mutex g_workerMutex;
bool g_workerSucceeded = false;
std::wstring g_workerError;

MediaPlayer* g_player = nullptr; // process lifetime, see CreateMediaPlayer()

// ---------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------

const wchar_t* ModeName(int mode)
{
    switch (mode)
    {
    case 0:
        return L"只录制系统声音";
    case 1:
        return L"只录制麦克风声音";
    default:
        return L"混合录制（系统声音 + 麦克风）";
    }
}

int CurrentMode()
{
    for (int index = 0; index < 3; ++index)
    {
        if (SendMessageW(g_modeRadios[index], BM_GETCHECK, 0, 0) == BST_CHECKED)
        {
            return index;
        }
    }
    return 2;
}

std::wstring FormatDuration(UINT64 milliseconds)
{
    const UINT64 totalSeconds = milliseconds / 1000;
    const UINT64 hours = totalSeconds / 3600;
    const UINT64 minutes = (totalSeconds % 3600) / 60;
    const UINT64 seconds = totalSeconds % 60;

    wchar_t buffer[64];
    if (hours > 0)
    {
        swprintf_s(buffer, L"%llu:%02llu:%02llu", hours, minutes, seconds);
    }
    else
    {
        swprintf_s(buffer, L"%02llu:%02llu", minutes, seconds);
    }
    return buffer;
}

std::wstring FormatFileSize(UINT64 bytes)
{
    const wchar_t* const units[] = {L"B", L"KB", L"MB", L"GB"};
    double size = static_cast<double>(bytes);
    int unit = 0;
    while (size >= 1024.0 && unit < 3)
    {
        size /= 1024.0;
        ++unit;
    }
    wchar_t buffer[64];
    swprintf_s(buffer, L"%.2f %s", size, units[unit]);
    return buffer;
}

bool EnsureDirectory(const std::wstring& path)
{
    if (path.empty())
    {
        return false;
    }
    const int result = SHCreateDirectoryExW(nullptr, path.c_str(), nullptr);
    return result == ERROR_SUCCESS || result == ERROR_ALREADY_EXISTS ||
           result == ERROR_FILE_EXISTS;
}

bool DeleteTree(const std::wstring& path)
{
    if (path.empty())
    {
        return true;
    }

    const std::wstring pattern = path + L"\\*";
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW(pattern.c_str(), &data);
    if (find != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0)
            {
                continue;
            }
            const std::wstring child = path + L"\\" + data.cFileName;
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
            {
                DeleteTree(child);
            }
            else
            {
                SetFileAttributesW(child.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(child.c_str());
            }
        } while (FindNextFileW(find, &data) != FALSE);
        FindClose(find);
    }

    return RemoveDirectoryW(path.c_str()) != FALSE;
}

std::wstring TempRoot()
{
    wchar_t buffer[MAX_PATH] = {};
    if (GetTempPathW(MAX_PATH, buffer) == 0)
    {
        return L"";
    }
    std::wstring path = buffer;
    if (!path.empty() && path.back() == L'\\')
    {
        path.pop_back();
    }
    path += L"\\";
    path += kTempFolderName;
    EnsureDirectory(path);
    return path;
}

// Shows temporary paths as "%TEMP%\..." so the status text stays readable while
// still being a path the user can paste into Explorer.
std::wstring DisplayPath(const std::wstring& fullPath)
{
    wchar_t buffer[MAX_PATH] = {};
    if (GetTempPathW(MAX_PATH, buffer) == 0)
    {
        return fullPath;
    }

    std::wstring root = buffer;
    if (!root.empty() && root.back() == L'\\')
    {
        root.pop_back();
    }

    if (!root.empty() && fullPath.size() > root.size() &&
        _wcsnicmp(fullPath.c_str(), root.c_str(), root.size()) == 0)
    {
        return std::wstring(L"%TEMP%") + fullPath.substr(root.size());
    }
    return fullPath;
}

std::wstring NewSessionDir()
{
    SYSTEMTIME time{};
    GetLocalTime(&time);

    wchar_t name[96];
    swprintf_s(name, L"rec-%04d%02d%02d-%02d%02d%02d-%03d",
               time.wYear, time.wMonth, time.wDay,
               time.wHour, time.wMinute, time.wSecond, time.wMilliseconds);

    std::wstring path = TempRoot();
    if (path.empty())
    {
        return L"";
    }
    path += L"\\";
    path += name;
    return EnsureDirectory(path) ? path : L"";
}

// Removes leftover session folders from runs that were killed or that crashed.
// Only folders untouched for more than a day are removed, so a recording that is
// still in progress (in this or another instance) is never affected.
void CleanupStaleSessions()
{
    const std::wstring root = TempRoot();
    if (root.empty())
    {
        return;
    }

    FILETIME currentTime{};
    GetSystemTimeAsFileTime(&currentTime);
    ULARGE_INTEGER now{};
    now.LowPart = currentTime.dwLowDateTime;
    now.HighPart = currentTime.dwHighDateTime;

    const ULONGLONG maxAge = 24ULL * 60ULL * 60ULL * 10000000ULL; // 24 hours, 100 ns units

    WIN32_FIND_DATAW data{};
    const std::wstring pattern = root + L"\\rec-*";
    HANDLE find = FindFirstFileW(pattern.c_str(), &data);
    if (find == INVALID_HANDLE_VALUE)
    {
        return;
    }

    do
    {
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
        {
            continue;
        }

        ULARGE_INTEGER written{};
        written.LowPart = data.ftLastWriteTime.dwLowDateTime;
        written.HighPart = data.ftLastWriteTime.dwHighDateTime;

        if (now.QuadPart > written.QuadPart && (now.QuadPart - written.QuadPart) > maxAge)
        {
            DeleteTree(root + L"\\" + data.cFileName);
        }
    } while (FindNextFileW(find, &data) != FALSE);

    FindClose(find);
}

std::wstring DefaultSaveName(int mode)
{
    SYSTEMTIME time{};
    GetLocalTime(&time);

    const wchar_t* prefix = (mode == 0) ? L"系统声音" : (mode == 1) ? L"麦克风" : L"混合录音";

    wchar_t name[128];
    swprintf_s(name, L"%s_%04d-%02d-%02d_%02d-%02d-%02d.m4a", prefix,
               time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond);
    return name;
}

// ---------------------------------------------------------------------------
// UI plumbing
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// DPI helpers: the layout is authored for 96 DPI and scaled at runtime, so the
// text never outgrows its controls on high-DPI displays.
// ---------------------------------------------------------------------------

UINT QuerySystemDpi()
{
    HDC dc = GetDC(nullptr);
    const UINT dpi = dc != nullptr ? static_cast<UINT>(GetDeviceCaps(dc, LOGPIXELSY)) : 96;
    if (dc != nullptr)
    {
        ReleaseDC(nullptr, dc);
    }
    return dpi != 0 ? dpi : 96;
}

UINT QueryWindowDpi(HWND window)
{
    typedef UINT(WINAPI * GetDpiForWindowFn)(HWND);
    static GetDpiForWindowFn getDpiForWindow = reinterpret_cast<GetDpiForWindowFn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));

    UINT dpi = (getDpiForWindow != nullptr && window != nullptr) ? getDpiForWindow(window) : 0;
    return dpi != 0 ? dpi : QuerySystemDpi();
}

int Scale(int value)
{
    return MulDiv(value, static_cast<int>(g_dpi), 96);
}

// One UI font for everything (labels, mode radios, all three buttons): the
// user's message font, rescaled for the monitor's DPI.
HFONT CreateUiFont(UINT dpi)
{
    LOGFONTW logFont{};
    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);

    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0))
    {
        logFont = metrics.lfMessageFont;
        const UINT systemDpi = QuerySystemDpi();
        logFont.lfHeight = MulDiv(logFont.lfHeight, static_cast<int>(dpi),
                                  static_cast<int>(systemDpi));
    }
    else
    {
        wcscpy_s(logFont.lfFaceName, L"Segoe UI");
        logFont.lfHeight = -MulDiv(9, static_cast<int>(dpi), 72);
    }

    logFont.lfWeight = FW_NORMAL;

    return CreateFontIndirectW(&logFont);
}

int LineHeight(HFONT font)
{
    if (font == nullptr)
    {
        return Scale(20);
    }

    HDC dc = GetDC(g_window);
    HGDIOBJ previous = SelectObject(dc, font);
    TEXTMETRICW metrics{};
    GetTextMetricsW(dc, &metrics);
    SelectObject(dc, previous);
    ReleaseDC(g_window, dc);

    const int height = metrics.tmHeight + metrics.tmExternalLeading;
    return height > 0 ? height : Scale(20);
}

int MeasureTextHeight(HWND control, HFONT font, const wchar_t* text, int width)
{
    if (text == nullptr || width <= 0)
    {
        return Scale(20);
    }

    HDC dc = GetDC(control);
    HGDIOBJ previous = (font != nullptr) ? SelectObject(dc, font) : nullptr;
    RECT rectangle = {0, 0, width, 0};
    DrawTextW(dc, text, -1, &rectangle, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX | DT_EXPANDTABS);
    if (previous != nullptr)
    {
        SelectObject(dc, previous);
    }
    ReleaseDC(control, dc);

    const int height = rectangle.bottom - rectangle.top;
    return height > 0 ? height : Scale(20);
}

int MeasureTextWidth(HWND control, HFONT font, const wchar_t* text)
{
    if (text == nullptr)
    {
        return 0;
    }

    HDC dc = GetDC(control);
    HGDIOBJ previous = (font != nullptr) ? SelectObject(dc, font) : nullptr;
    RECT rectangle = {0, 0, 0, 0};
    DrawTextW(dc, text, -1, &rectangle, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX | DT_EXPANDTABS);

    // Cross-check with GetTextExtentPoint32W (independent of DrawText's rect handling).
    SIZE extent{};
    GetTextExtentPoint32W(dc, text, static_cast<int>(wcslen(text)), &extent);

    if (previous != nullptr)
    {
        SelectObject(dc, previous);
    }
    ReleaseDC(control, dc);

    const int drawTextWidth = rectangle.right - rectangle.left;
    return std::max(drawTextWidth, static_cast<int>(extent.cx));
}

void UpdateMetrics()
{
    g_lineHeight = LineHeight(g_font);
    g_hintHeight = std::max(LineHeight(g_font) * 2 + Scale(8),
                            MeasureTextHeight(g_hintText, g_font, kHintText,
                                              Scale(kMinimumClientWidth) - Scale(16) * 2) +
                                Scale(8));
}

// Smallest client area that still shows every control without clipping text.
// Both dimensions are measured from the real font instead of guessed, so this
// stays correct for any DPI, font size or UI language.
void ComputeMinimumClientSize(int& width, int& height)
{
    const int margin = Scale(16);
    const int lineHeight = (g_lineHeight > 0) ? g_lineHeight : Scale(20);

    // The mode labels are single-line radio buttons, so the widest one has to fit.
    int radioTextWidth = 0;
    for (const wchar_t* label : kModeLabels)
    {
        radioTextWidth = std::max(radioTextWidth, MeasureTextWidth(g_modeRadios[2], g_font, label));
    }

    width = std::max(Scale(kMinimumClientWidth),
                     radioTextWidth + Scale(40) /* group inset + button circle */ +
                         Scale(28) /* slack */ + margin * 2);

    const int contentWidth = width - margin * 2;

    // Reserve enough room for the longest possible status text and for the hint;
    // the extra slack absorbs differences in font rendering.
    const int statusReserve = std::max(
        lineHeight * 4 + Scale(8),
        MeasureTextHeight(g_statusText, g_font, kWorstCaseStatusText, contentWidth) + Scale(8));
    const int hintReserve = std::max(
        lineHeight * 2 + Scale(8),
        MeasureTextHeight(g_hintText, g_font, kHintText, contentWidth) + Scale(8));

    g_hintHeight = hintReserve;

    height = Scale(10)       // top margin
           + Scale(118)      // mode group box
           + Scale(24)       // gap
           + Scale(48)       // button row
           + Scale(14)       // gap
           + statusReserve   // status text (worst case)
           + Scale(12)       // gap
           + hintReserve     // format hint
           + margin;         // bottom margin

    Trace(L"layout: dpi=" + std::to_wstring(g_dpi) + L" systemDpi=" +
          std::to_wstring(QuerySystemDpi()) + L" lineHeight=" + std::to_wstring(lineHeight) +
          L" radioText=" + std::to_wstring(radioTextWidth) + L" hintReserve=" +
          std::to_wstring(hintReserve) + L" statusReserve=" + std::to_wstring(statusReserve) +
          L" min=" + std::to_wstring(width) + L"x" + std::to_wstring(height));
}

// Drives the client area to exactly the requested size.  Measuring the result
// instead of trusting AdjustWindowRectEx keeps this correct on every DPI and
// Windows version.
void EnsureClientSize(HWND window, int desiredWidth, int desiredHeight)
{
    if (window == nullptr)
    {
        return;
    }

    for (int attempt = 0; attempt < 4; ++attempt)
    {
        RECT client{};
        GetClientRect(window, &client);
        const int deltaWidth = desiredWidth - (client.right - client.left);
        const int deltaHeight = desiredHeight - (client.bottom - client.top);
        if (deltaWidth == 0 && deltaHeight == 0)
        {
            return;
        }

        RECT windowRect{};
        GetWindowRect(window, &windowRect);
        SetWindowPos(window, nullptr, 0, 0,
                     std::max(120, static_cast<int>(windowRect.right - windowRect.left) + deltaWidth),
                     std::max(120, static_cast<int>(windowRect.bottom - windowRect.top) + deltaHeight),
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

RECT WindowRectForClient(HWND window, int clientWidth, int clientHeight)
{
    RECT rectangle = {0, 0, clientWidth, clientHeight};
    const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE));
    const DWORD extendedStyle = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_EXSTYLE));

    typedef BOOL(WINAPI * AdjustWindowRectExForDpiFn)(LPRECT, DWORD, BOOL, DWORD, UINT);
    static AdjustWindowRectExForDpiFn adjustForDpi = reinterpret_cast<AdjustWindowRectExForDpiFn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "AdjustWindowRectExForDpi"));

    if (adjustForDpi != nullptr)
    {
        adjustForDpi(&rectangle, style, FALSE, extendedStyle, g_dpi);
    }
    else
    {
        AdjustWindowRectEx(&rectangle, style, FALSE, extendedStyle);
    }
    return rectangle;
}

// Vertical flow layout.  The status box absorbs all extra height, so the window
// can be resized freely without clipping any text.
void LayoutControls()
{
    if (g_window == nullptr || g_groupMode == nullptr)
    {
        return;
    }

    RECT client{};
    GetClientRect(g_window, &client);
    const int clientWidth = client.right - client.left;
    const int clientHeight = client.bottom - client.top;

    const int margin = Scale(16);
    const int contentWidth = std::max(Scale(240), clientWidth - margin * 2);

    const int groupTop = Scale(10);
    const int groupHeight = Scale(118);
    SetWindowPos(g_groupMode, nullptr, margin, groupTop, contentWidth, groupHeight,
                 SWP_NOZORDER | SWP_NOACTIVATE);

    const int radioTop[3] = {Scale(38), Scale(68), Scale(98)};
    const int radioWidth = std::max(Scale(140), contentWidth - Scale(40));
    for (int index = 0; index < 3; ++index)
    {
        SetWindowPos(g_modeRadios[index], nullptr, margin + Scale(20), radioTop[index],
                     radioWidth, Scale(22), SWP_NOZORDER | SWP_NOACTIVATE);
    }

    const int buttonTop = groupTop + groupHeight + Scale(24);
    const int buttonHeight = Scale(48);
    const int gap = Scale(12);

    // The three buttons are always the same size.
    const int buttonWidth = std::max(Scale(96), (contentWidth - gap * 2) / 3);

    SetWindowPos(g_recordButton, nullptr, margin, buttonTop, buttonWidth, buttonHeight,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(g_playButton, nullptr, margin + buttonWidth + gap, buttonTop, buttonWidth,
                 buttonHeight, SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(g_saveButton, nullptr, margin + (buttonWidth + gap) * 2, buttonTop, buttonWidth,
                 buttonHeight, SWP_NOZORDER | SWP_NOACTIVATE);

    const int hintHeight = std::max(
        (g_hintHeight > 0) ? g_hintHeight : Scale(40),
        MeasureTextHeight(g_hintText, g_font, kHintText, contentWidth) + Scale(4));
    const int hintTop = std::max(buttonTop + buttonHeight, clientHeight - margin - hintHeight);
    SetWindowPos(g_hintText, nullptr, margin, hintTop, contentWidth, hintHeight,
                 SWP_NOZORDER | SWP_NOACTIVATE);

    const int statusTop = buttonTop + buttonHeight + Scale(14);
    const int statusHeight = std::max(0, hintTop - Scale(12) - statusTop);
    SetWindowPos(g_statusText, nullptr, margin, statusTop, contentWidth, statusHeight,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

HWND CreateControl(const wchar_t* className, const wchar_t* text, DWORD style,
                   int id, DWORD extendedStyle = 0)
{
    return CreateWindowExW(extendedStyle, className, text, style, 0, 0, 0, 0, g_window,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                           g_instance, nullptr);
}

void ApplyFonts()
{
    const int controlCount = 9;
    HWND controls[controlCount] = {
        g_groupMode, g_modeRadios[0], g_modeRadios[1], g_modeRadios[2],
        g_recordButton, g_playButton, g_saveButton, g_statusText, g_hintText};

    for (int index = 0; index < controlCount; ++index)
    {
        if (controls[index] == nullptr)
        {
            continue;
        }
        SendMessageW(controls[index], WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
    }
}

void ApplyDpi(UINT dpi)
{
    g_dpi = dpi;

    HFONT const previousFont = g_font;
    g_font = CreateUiFont(dpi);

    ApplyFonts();
    UpdateMetrics();

    // Keep the user's window size, but never smaller than the content needs.
    int minimumWidth = 0;
    int minimumHeight = 0;
    ComputeMinimumClientSize(minimumWidth, minimumHeight);

    RECT client{};
    GetClientRect(g_window, &client);
    EnsureClientSize(g_window,
                     std::max(minimumWidth, static_cast<int>(client.right - client.left)),
                     std::max(minimumHeight, static_cast<int>(client.bottom - client.top)));

    LayoutControls();

    if (previousFont != nullptr)
    {
        DeleteObject(previousFont);
    }
}

// ---------------------------------------------------------------------------
// controls
// ---------------------------------------------------------------------------

void CreateControls()
{
    const DWORD radioStyle = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON;

    g_groupMode = CreateControl(L"BUTTON", L"录制模式",
                                WS_CHILD | WS_VISIBLE | BS_GROUPBOX, IDC_GROUP_MODE);

    g_modeRadios[0] = CreateControl(L"BUTTON", kModeLabels[0],
                                    radioStyle | WS_GROUP, IDC_MODE_SYSTEM);
    g_modeRadios[1] = CreateControl(L"BUTTON", kModeLabels[1],
                                    radioStyle, IDC_MODE_MIC);
    g_modeRadios[2] = CreateControl(L"BUTTON", kModeLabels[2],
                                    radioStyle, IDC_MODE_MIX);

    SendMessageW(g_modeRadios[2], BM_SETCHECK, BST_CHECKED, 0);

    g_recordButton = CreateControl(L"BUTTON", L"●  开始录制",
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | WS_GROUP,
                                   IDC_RECORD_BTN);
    g_playButton = CreateControl(L"BUTTON", L"▶  回放",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                 IDC_PLAY_BTN);
    g_saveButton = CreateControl(L"BUTTON", L"保存为 M4A…",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                 IDC_SAVE_BTN);

    g_statusText = CreateControl(L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                 IDC_STATUS_TEXT);
    g_hintText = CreateControl(L"STATIC", kHintText, WS_CHILD | WS_VISIBLE | SS_LEFT,
                               IDC_HINT_TEXT);
}

void UpdateStatusText()
{
    std::wstring text;

    switch (g_state)
    {
    case State::Idle:
        text = L"就绪。请选择录制模式，然后点击“开始录制”。";
        break;

    case State::Recording:
    {
        const UINT64 elapsed = GetTickCount64() - g_recordStartTick;
        text = L"● 正在录制：";
        text += ModeName(CurrentMode());
        text += L"\n已录制时间：";
        text += FormatDuration(elapsed);
        text += L"        （再次点击按钮即可停止）";
        break;
    }

    case State::Processing:
        text = L"正在停止录音并使用系统原生管线合成 M4A…\n请稍候，长录音可能需要几秒钟。";
        break;

    case State::Ready:
    {
        UINT64 fileSize = 0;
        WIN32_FILE_ATTRIBUTE_DATA attributes{};
        if (GetFileAttributesExW(g_resultPath.c_str(), GetFileExInfoStandard, &attributes))
        {
            fileSize = (static_cast<UINT64>(attributes.nFileSizeHigh) << 32) | attributes.nFileSizeLow;
        }

        text = L"✔ 录音完成：";
        text += ModeName(g_lastMode);
        text += g_resultSaved ? L"（已保存）" : L"（尚未保存）";
        text += L"\n时长 ";
        text += FormatDuration(g_resultDurationMs);
        text += L"，文件大小 ";
        text += FormatFileSize(fileSize);
        if (!g_resultSaved)
        {
            text += L"\n临时文件：";
            text += DisplayPath(g_resultPath);
            text += L"（退出时删除）";
        }
        if (!g_captureNote.empty())
        {
            text += L"\n";
            text += g_captureNote;
        }
        text += L"\n可回放试听、另存为 M4A，或直接点“开始录制”录下一段。";
        break;
    }
    }

    SetWindowTextW(g_statusText, text.c_str());
}

void UpdateUi()
{
    const bool idle = (g_state == State::Idle);
    const bool ready = (g_state == State::Ready);
    const bool recording = (g_state == State::Recording);
    const bool processing = (g_state == State::Processing);

    // The record button doubles as stop and as "start the next recording"; only
    // the (short) encoding step blocks it.
    EnableWindow(g_recordButton, !processing);
    EnableWindow(g_playButton, ready);
    EnableWindow(g_saveButton, ready);

    // Modes may be changed while idle and while a finished recording is waiting
    // to be saved/played.
    for (HWND radio : g_modeRadios)
    {
        EnableWindow(radio, idle || ready);
    }

    SetWindowTextW(g_recordButton, recording ? L"■  停止录制" : L"●  开始录制");
    SetWindowTextW(g_playButton, (g_player != nullptr && g_player->IsPlaying())
                                     ? L"■  停止回放"
                                     : L"▶  回放");

    UpdateStatusText();
}

void CleanupSession()
{
    if (!g_sessionDir.empty())
    {
        DeleteTree(g_sessionDir);
        g_sessionDir.clear();
    }
    g_resultPath.clear();
    g_resultDurationMs = 0;
}

void ReleaseEngines()
{
    if (g_systemEngine)
    {
        g_systemEngine->Stop();
        g_systemEngine.reset();
    }
    if (g_micEngine)
    {
        g_micEngine->Stop();
        g_micEngine.reset();
    }
}

void StopPlayback()
{
    if (g_player != nullptr)
    {
        g_player->Stop();
    }
}

// ---------------------------------------------------------------------------
// recording
// ---------------------------------------------------------------------------

void StartRecording()
{
    // Starting over discards the previous (possibly unsaved) recording.
    if (g_state == State::Ready && !g_resultPath.empty() && !g_resultSaved)
    {
        const int answer = MessageBoxW(
            g_window,
            L"上一次的录音还没有保存，开始新的录音会丢弃它。\n是否继续？",
            L"确认开始新录音", MB_YESNO | MB_ICONQUESTION);
        if (answer != IDYES)
        {
            return;
        }
    }

    StopPlayback();
    CleanupSession();
    ReleaseEngines();
    g_resultSaved = false;
    g_captureNote.clear();

    const int mode = CurrentMode();
    const bool wantSystem = (mode == 0 || mode == 2);
    const bool wantMic = (mode == 1 || mode == 2);

    g_sessionDir = NewSessionDir();
    if (g_sessionDir.empty())
    {
        MessageBoxW(g_window, L"无法创建临时目录，请检查系统临时文件夹是否可写。",
                    L"启动录音失败", MB_OK | MB_ICONERROR);
        return;
    }

    std::wstring error;

    if (wantSystem)
    {
        g_systemEngine = std::make_unique<AudioCaptureEngine>();
        if (!g_systemEngine->Open(CaptureRoute::SystemLoopback,
                                  g_sessionDir + L"\\system.wav", error) ||
            !g_systemEngine->Start(error))
        {
            ReleaseEngines();
            CleanupSession();
            MessageBoxW(g_window, error.c_str(), L"无法录制系统声音", MB_OK | MB_ICONERROR);
            return;
        }
    }

    if (wantMic)
    {
        g_micEngine = std::make_unique<AudioCaptureEngine>();
        if (!g_micEngine->Open(CaptureRoute::Microphone,
                               g_sessionDir + L"\\microphone.wav", error) ||
            !g_micEngine->Start(error))
        {
            ReleaseEngines();
            CleanupSession();
            MessageBoxW(g_window, error.c_str(), L"无法录制麦克风声音", MB_OK | MB_ICONERROR);
            return;
        }
    }

    g_lastMode = mode;
    g_recordStartTick = GetTickCount64();
    g_state = State::Recording;
    SetTimer(g_window, IDT_ELAPSED, 500, nullptr);
    UpdateUi();
}

void StopRecording()
{
    KillTimer(g_window, IDT_ELAPSED);

    g_state = State::Processing;
    UpdateUi();

    // Both routes are closed here so that the worker thread only reads files.
    std::vector<mfaudio::InputFile> inputs;
    if (g_systemEngine)
    {
        g_systemEngine->Stop();
        inputs.push_back({g_systemEngine->TempWavPath(), g_systemEngine->BaseQpc()});
    }
    if (g_micEngine)
    {
        g_micEngine->Stop();
        inputs.push_back({g_micEngine->TempWavPath(), g_micEngine->BaseQpc()});
    }

    const std::wstring outputPath = g_sessionDir + L"\\result.m4a";

    {
        std::lock_guard<std::mutex> lock(g_workerMutex);
        g_workerSucceeded = false;
        g_workerError.clear();
    }

    g_worker = std::thread([inputs, outputPath]() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        mfaudio::Startup();

        std::wstring error;
        mfaudio::MixResult result;
        const bool ok = mfaudio::MixToM4a(inputs, outputPath, kAacBitrate, error, &result);

        mfaudio::Shutdown();
        CoUninitialize();

        {
            std::lock_guard<std::mutex> lock(g_workerMutex);
            g_workerSucceeded = ok;
            g_workerError = error;
            g_resultDurationMs = result.durationMs;
        }

        PostMessageW(g_window, WM_APP_PROCESS_DONE, 0, 0);
    });
}

void OnProcessDone()
{
    if (g_worker.joinable())
    {
        g_worker.join();
    }

    bool succeeded = false;
    std::wstring error;
    {
        std::lock_guard<std::mutex> lock(g_workerMutex);
        succeeded = g_workerSucceeded;
        error = g_workerError;
    }

    if (succeeded)
    {
        // Collect any capture/file problem before the engines are destroyed.
        bool deviceLost = false;
        bool writeFailed = false;
        bool captureProblem = false;
        UINT64 droppedFrames = 0;
        UINT32 droppedRate = 48000;
        bool silentSystemRoute = false;
        bool silentMicRoute = false;
        for (const AudioCaptureEngine* engine : {g_systemEngine.get(), g_micEngine.get()})
        {
            if (engine == nullptr)
            {
                continue;
            }
            deviceLost = deviceLost || engine->CaptureFailed();
            writeFailed = writeFailed || engine->WriteFailed();
            captureProblem = captureProblem || engine->HasProblem();
            if (engine->DroppedFrames() > droppedFrames)
            {
                droppedFrames = engine->DroppedFrames();
                droppedRate = engine->CaptureFormat() != nullptr
                                  ? engine->CaptureFormat()->nSamplesPerSec
                                  : 48000;
            }

            // "No signal" is either "not a single packet arrived" (loopback with
            // silence) or "packets arrived but every sample was zero".
            const bool silent = engine->FramesCaptured() == 0 ||
                                (engine->PeakLevelValid() && engine->PeakLevel() == 0);
            if (silent && engine == g_systemEngine.get())
            {
                silentSystemRoute = true;
            }
            if (silent && engine == g_micEngine.get())
            {
                silentMicRoute = true;
            }
        }

        if (droppedFrames > 0 && droppedRate > 0)
        {
            wchar_t note[160];
            swprintf_s(note, L"⚠ 录音设备返回了异常时间戳，已按真实录制时长忽略 %.1f 秒静音",
                       static_cast<double>(droppedFrames) / droppedRate);
            g_captureNote = note;
        }
        else
        {
            g_captureNote.clear();
        }

        if (silentMicRoute)
        {
            if (!g_captureNote.empty())
            {
                g_captureNote += L"\n";
            }
            g_captureNote += L"⚠ 麦克风轨全程没有声音（检查是否被静音／选错设备／静音键）";
        }
        if (silentSystemRoute)
        {
            if (!g_captureNote.empty())
            {
                g_captureNote += L"\n";
            }
            g_captureNote += L"⚠ 系统声音轨全程没有声音（录制时没有任何程序在播放？）";
        }

        g_resultPath = g_sessionDir + L"\\result.m4a";

        // The intermediate PCM files are no longer needed.
        if (g_systemEngine)
        {
            DeleteFileW(g_systemEngine->TempWavPath().c_str());
        }
        if (g_micEngine)
        {
            DeleteFileW(g_micEngine->TempWavPath().c_str());
        }
        ReleaseEngines();

        g_state = State::Ready;
        UpdateUi();

        if (captureProblem)
        {
            std::wstring message = L"录音过程中出现了问题：\n\n";
            if (deviceLost)
            {
                message += L"· 录音设备中途不可用（被拔出、禁用或被其他程序独占）\n";
            }
            if (writeFailed)
            {
                message += L"· 临时文件写入失败（磁盘空间不足，或单文件超过 4 GB 上限）\n";
            }
            message += L"\n已生成的文件可能不完整，但可以正常回放和保存。";
            MessageBoxW(g_window, message.c_str(), L"录音可能不完整", MB_OK | MB_ICONWARNING);
        }
    }
    else
    {
        ReleaseEngines();
        CleanupSession();
        g_state = State::Idle;
        UpdateUi();

        if (error.empty())
        {
            error = L"未知错误。";
        }
        MessageBoxW(g_window, error.c_str(), L"合成失败", MB_OK | MB_ICONERROR);
    }
}

void TogglePlayback()
{
    if (g_player == nullptr || g_resultPath.empty())
    {
        return;
    }

    if (g_player->IsPlaying())
    {
        StopPlayback();
        UpdateUi();
        return;
    }

    std::wstring error;
    if (!g_player->PlayFile(g_resultPath, error))
    {
        // Last resort: let the shell open the file with the default player.
        const HINSTANCE result = ShellExecuteW(g_window, L"open", g_resultPath.c_str(),
                                               nullptr, nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(result) <= 32)
        {
            MessageBoxW(g_window, error.c_str(), L"无法回放", MB_OK | MB_ICONERROR);
        }
        return;
    }

    UpdateUi();
}

bool SaveResult()
{
    if (g_resultPath.empty())
    {
        return false;
    }

    IFileSaveDialog* dialog = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&dialog));
    if (FAILED(hr) || dialog == nullptr)
    {
        MessageBoxW(g_window, L"无法打开保存对话框。", L"保存失败", MB_OK | MB_ICONERROR);
        return false;
    }

    const COMDLG_FILTERSPEC filters[] = {{L"M4A 音频文件 (*.m4a)", L"*.m4a"}};
    dialog->SetFileTypes(1, filters);
    dialog->SetDefaultExtension(L"m4a");
    dialog->SetTitle(L"保存录音");

    const std::wstring suggestedName = DefaultSaveName(g_lastMode);
    dialog->SetFileName(suggestedName.c_str());

    IShellItem* folder = nullptr;
    if (SUCCEEDED(SHCreateItemInKnownFolder(FOLDERID_Music, 0, nullptr, IID_PPV_ARGS(&folder))) &&
        folder != nullptr)
    {
        dialog->SetDefaultFolder(folder);
        dialog->SetFolder(folder);
        folder->Release();
    }

    std::wstring destination;
    if (SUCCEEDED(dialog->Show(g_window)))
    {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item)) && item != nullptr)
        {
            LPWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path != nullptr)
            {
                destination = path;
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }

    dialog->Release();

    if (destination.empty())
    {
        return false; // cancelled
    }

    StopPlayback();

    if (!CopyFileW(g_resultPath.c_str(), destination.c_str(), FALSE))
    {
        const DWORD lastError = GetLastError();
        wchar_t message[512];
        swprintf_s(message, L"无法保存文件（错误 %lu）。\n目标：%s",
                   lastError, destination.c_str());
        MessageBoxW(g_window, message, L"保存失败", MB_OK | MB_ICONERROR);
        return false;
    }

    g_resultSaved = true;

    std::wstring message = L"已保存：\n";
    message += destination;
    MessageBoxW(g_window, message.c_str(), L"保存成功", MB_OK | MB_ICONINFORMATION);

    UpdateUi();
    return true;
}

// ---------------------------------------------------------------------------
// window procedure
// ---------------------------------------------------------------------------

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_CREATE:
        g_window = window;
        g_dpi = QueryWindowDpi(window);
        g_font = CreateUiFont(g_dpi);
        CreateControls();
        ApplyFonts();
        UpdateMetrics();
        {
            int minimumWidth = 0;
            int minimumHeight = 0;
            ComputeMinimumClientSize(minimumWidth, minimumHeight);
            EnsureClientSize(window, minimumWidth, minimumHeight + Scale(kExtraStatusHeight));
        }
        LayoutControls();
        UpdateUi();
        return 0;

    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED)
        {
            LayoutControls();
        }
        return 0;

    case WM_GETMINMAXINFO:
    {
        int minimumWidth = 0;
        int minimumHeight = 0;
        ComputeMinimumClientSize(minimumWidth, minimumHeight);

        const RECT rectangle = WindowRectForClient(window, minimumWidth, minimumHeight);
        MINMAXINFO* info = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize.x = rectangle.right - rectangle.left;
        info->ptMinTrackSize.y = rectangle.bottom - rectangle.top;
        return 0;
    }

    case WM_DPICHANGED:
    {
        const UINT newDpi = LOWORD(wParam);
        ApplyDpi(newDpi != 0 ? newDpi : QueryWindowDpi(window));

        const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
        if (suggested != nullptr)
        {
            SetWindowPos(window, nullptr, suggested->left, suggested->top, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDC_RECORD_BTN:
            if (HIWORD(wParam) == BN_CLICKED)
            {
                if (g_state == State::Recording)
                {
                    StopRecording();
                }
                else if (g_state == State::Idle || g_state == State::Ready)
                {
                    StartRecording();
                }
            }
            return 0;

        case IDC_PLAY_BTN:
            if (HIWORD(wParam) == BN_CLICKED)
            {
                TogglePlayback();
            }
            return 0;

        case IDC_SAVE_BTN:
            if (HIWORD(wParam) == BN_CLICKED)
            {
                (void)SaveResult();
            }
            return 0;

        default:
            break;
        }
        break;

    case WM_TIMER:
        if (wParam == IDT_ELAPSED && g_state == State::Recording)
        {
            UpdateStatusText();
        }
        return 0;

    case WM_APP_PROCESS_DONE:
        OnProcessDone();
        return 0;

    case WM_APP_PLAYBACK_ENDED:
        if (g_player != nullptr)
        {
            if (g_player->TakeError())
            {
                MessageBoxW(g_window, L"回放过程中出现错误。", L"回放失败",
                            MB_OK | MB_ICONWARNING);
            }
            g_player->Stop();
        }
        UpdateUi();
        return 0;

    case WM_CLOSE:
        if (g_state == State::Recording)
        {
            const int answer = MessageBoxW(g_window,
                                           L"正在录音，确定要停止并退出吗？未保存的录音将丢失。",
                                           L"确认退出", MB_YESNO | MB_ICONQUESTION);
            if (answer != IDYES)
            {
                return 0;
            }
            KillTimer(g_window, IDT_ELAPSED);
            ReleaseEngines();
            CleanupSession();
        }
        else if (g_state == State::Processing)
        {
            MessageBoxW(g_window, L"正在合成录音，请稍候再关闭窗口。",
                        L"请稍候", MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        else if (g_state == State::Ready && !g_resultSaved)
        {
            const int answer = MessageBoxW(
                g_window,
                L"这次录音还没有保存。\n\n"
                L"“是”：现在保存为 M4A 文件\n"
                L"“否”：丢弃并退出（临时文件会被删除）\n"
                L"“取消”：返回程序",
                L"录音尚未保存", MB_YESNOCANCEL | MB_ICONQUESTION);

            if (answer == IDCANCEL)
            {
                return 0;
            }
            if (answer == IDYES && !SaveResult())
            {
                // Saving was cancelled or failed: keep the recording.
                return 0;
            }
        }

        DestroyWindow(window);
        return 0;

    case WM_DESTROY:
        KillTimer(g_window, IDT_ELAPSED);
        StopPlayback();
        if (g_worker.joinable())
        {
            g_worker.join();
        }
        if (g_player != nullptr)
        {
            g_player->Shutdown();
        }
        // COM objects must be released before CoUninitialize() at the end of
        // RunApplication, so do it here and not only in the destructors.
        if (g_systemEngine)
        {
            g_systemEngine->Close();
        }
        if (g_micEngine)
        {
            g_micEngine->Close();
        }
        ReleaseEngines();
        CleanupSession();
        PostQuitMessage(0);
        return 0;

    case WM_CTLCOLORSTATIC:
        SetBkMode(reinterpret_cast<HDC>(wParam), TRANSPARENT);
        return reinterpret_cast<LRESULT>(g_backgroundBrush);

    default:
        break;
    }

    return DefWindowProcW(window, message, wParam, lParam);
}

// ---------------------------------------------------------------------------
// self test (headless verification of capture + encode + playback)
// ---------------------------------------------------------------------------

struct SelfTestContext
{
    FILE* log = nullptr;
    HWND window = nullptr;
    bool playbackEnded = false;

    void Log(const std::wstring& text)
    {
        if (log != nullptr)
        {
            fwprintf(log, L"%s\n", text.c_str());
            fflush(log);
        }
    }
};

LRESULT CALLBACK SelfTestWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_APP_PLAYBACK_ENDED)
    {
        SelfTestContext* context =
            reinterpret_cast<SelfTestContext*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (context != nullptr)
        {
            context->playbackEnded = true;
        }
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

UINT32 PeakLevel(const std::wstring& wavPath)
{
    HANDLE file = CreateFileW(wavPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return 0;
    }

    LARGE_INTEGER size{};
    GetFileSizeEx(file, &size);
    if (size.QuadPart <= 44)
    {
        CloseHandle(file);
        return 0;
    }

    const DWORD dataBytes = static_cast<DWORD>(size.QuadPart - 44);
    std::vector<BYTE> data(dataBytes);
    SetFilePointer(file, 44, nullptr, FILE_BEGIN);
    DWORD read = 0;
    const BOOL readOk = ReadFile(file, data.data(), dataBytes, &read, nullptr);
    CloseHandle(file);

    if (!readOk || read < 2)
    {
        return 0;
    }

    const int16_t* samples = reinterpret_cast<const int16_t*>(data.data());
    const size_t count = read / 2;
    int peak = 0;
    for (size_t index = 0; index < count; ++index)
    {
        const int value = samples[index] < 0 ? -samples[index] : samples[index];
        if (value > peak)
        {
            peak = value;
        }
    }
    return static_cast<UINT32>(peak);
}

UINT64 FileSize(const std::wstring& path)
{
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes))
    {
        return 0;
    }
    return (static_cast<UINT64>(attributes.nFileSizeHigh) << 32) | attributes.nFileSizeLow;
}

// Deterministic check of the sample-format conversion the capture path relies on.
// A real device almost always reports 32-bit float, so without this the int32 /
// int24 / int8 / int16 branches would never be exercised.
bool RunWriterSelfTest(SelfTestContext& context, const std::wstring& outputDir)
{
    const UINT32 frames = 512;
    const UINT32 half = frames / 2;
    bool allOk = true;

    auto check = [&](const wchar_t* name, const WAVEFORMATEX& format, const BYTE* payload,
                     UINT32 payloadBytes, UINT32 expectPeak) {
        const std::wstring path = outputDir + L"\\writer_" + name + L".wav";
        DeleteFileW(path.c_str());

        WavWriter writer;
        if (!writer.Open(path, &format))
        {
            context.Log(std::wstring(L"写入器自检 ") + name + L": 无法创建文件");
            allOk = false;
            return;
        }

        writer.Write(payload, payloadBytes);
        writer.WriteSilenceFrames(16);
        const bool writeError = writer.HasWriteError();
        writer.Close();

        // Every case must end up as 16-bit PCM with the same channel count and
        // frame count, so the file size is fully predictable.
        const UINT64 expectBytes = 44ULL + (frames + 16ULL) * format.nChannels * 2ULL;
        const UINT64 bytes = FileSize(path);
        const UINT32 peak = PeakLevel(path);
        const bool peakOk = peak + 3 >= expectPeak && peak <= expectPeak + 3;
        const bool ok = !writeError && bytes == expectBytes && peakOk;
        if (!ok)
        {
            allOk = false;
        }

        context.Log(std::wstring(L"写入器自检 ") + name + L": " + (ok ? L"ok" : L"失败") +
                    L" (文件 " + std::to_wstring(bytes) + L" 字节/期望 " +
                    std::to_wstring(expectBytes) + L"，峰值 " + std::to_wstring(peak) +
                    L"/期望 " + std::to_wstring(expectPeak) + L")");
    };

    // 32-bit float, stereo
    {
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        format.nChannels = 2;
        format.nSamplesPerSec = 48000;
        format.wBitsPerSample = 32;
        format.nBlockAlign = static_cast<WORD>(format.nChannels * 4);
        format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

        std::vector<float> payload(static_cast<size_t>(frames) * 2, 0.0f);
        for (UINT32 index = half; index < frames; ++index)
        {
            payload[static_cast<size_t>(index) * 2] = 0.5f;
            payload[static_cast<size_t>(index) * 2 + 1] = -0.5f;
        }
        check(L"float32", format, reinterpret_cast<const BYTE*>(payload.data()),
              static_cast<UINT32>(payload.size() * sizeof(float)), 16384);
    }

    // 32-bit integer PCM, stereo (the branch that used to mis-count samples)
    {
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = 2;
        format.nSamplesPerSec = 48000;
        format.wBitsPerSample = 32;
        format.nBlockAlign = static_cast<WORD>(format.nChannels * 4);
        format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

        std::vector<int32_t> payload(static_cast<size_t>(frames) * 2, 0);
        for (UINT32 index = half; index < frames; ++index)
        {
            payload[static_cast<size_t>(index) * 2] = 536870912;  // 0.25 FS
            payload[static_cast<size_t>(index) * 2 + 1] = -536870912;
        }
        check(L"int32", format, reinterpret_cast<const BYTE*>(payload.data()),
              static_cast<UINT32>(payload.size() * sizeof(int32_t)), 8192);
    }

    // 24-bit PCM, mono
    {
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = 1;
        format.nSamplesPerSec = 44100;
        format.wBitsPerSample = 24;
        format.nBlockAlign = 3;
        format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

        std::vector<BYTE> payload(static_cast<size_t>(frames) * 3, 0);
        for (UINT32 index = half; index < frames; ++index)
        {
            const int32_t value = 4194304; // 0.5 FS in 24 bits
            payload[static_cast<size_t>(index) * 3] = static_cast<BYTE>(value & 0xFF);
            payload[static_cast<size_t>(index) * 3 + 1] = static_cast<BYTE>((value >> 8) & 0xFF);
            payload[static_cast<size_t>(index) * 3 + 2] = static_cast<BYTE>((value >> 16) & 0xFF);
        }
        check(L"int24", format, payload.data(), static_cast<UINT32>(payload.size()), 16384);
    }

    // 8-bit PCM, mono
    {
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = 1;
        format.nSamplesPerSec = 22050;
        format.wBitsPerSample = 8;
        format.nBlockAlign = 1;
        format.nAvgBytesPerSec = format.nSamplesPerSec;

        std::vector<BYTE> payload(frames, 128);
        for (UINT32 index = half; index < frames; ++index)
        {
            payload[index] = 192; // 0.5 FS
        }
        check(L"int8", format, payload.data(), static_cast<UINT32>(payload.size()), 16384);
    }

    // 16-bit PCM, stereo (pass-through)
    {
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = 2;
        format.nSamplesPerSec = 48000;
        format.wBitsPerSample = 16;
        format.nBlockAlign = static_cast<WORD>(format.nChannels * 2);
        format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

        std::vector<int16_t> payload(static_cast<size_t>(frames) * 2, 0);
        for (UINT32 index = half; index < frames; ++index)
        {
            payload[static_cast<size_t>(index) * 2] = 16384;
            payload[static_cast<size_t>(index) * 2 + 1] = -16384;
        }
        check(L"int16", format, reinterpret_cast<const BYTE*>(payload.data()),
              static_cast<UINT32>(payload.size() * sizeof(int16_t)), 16384);
    }

    return allOk;
}

// Decodes a compressed audio file with the Media Foundation source reader and
// returns the peak absolute 16-bit sample value.  Used by the self test to prove
// that the encoded files actually contain the recorded signal (a file can have
// the right size, the right duration and still be silent).
UINT32 DecodedPeakLevel(const std::wstring& path, UINT64* framesOut = nullptr)
{
    if (framesOut != nullptr)
    {
        *framesOut = 0;
    }

    IMFAttributes* attributes = nullptr;
    if (FAILED(MFCreateAttributes(&attributes, 1)))
    {
        return 0;
    }

    IMFSourceReader* reader = nullptr;
    HRESULT hr = MFCreateSourceReaderFromURL(path.c_str(), attributes, &reader);
    attributes->Release();
    if (FAILED(hr) || reader == nullptr)
    {
        return 0;
    }

    reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), TRUE);

    UINT32 rate = 48000;
    UINT32 channels = 2;
    IMFMediaType* native = nullptr;
    if (SUCCEEDED(reader->GetNativeMediaType(
            static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0, &native)) &&
        native != nullptr)
    {
        UINT32 value = 0;
        if (SUCCEEDED(native->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &value)) && value != 0)
        {
            rate = value;
        }
        if (SUCCEEDED(native->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &value)) && value != 0)
        {
            channels = value;
        }
        native->Release();
    }

    const UINT32 blockAlign = channels * 2;
    IMFMediaType* outputType = nullptr;
    if (SUCCEEDED(MFCreateMediaType(&outputType)))
    {
        outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        outputType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        outputType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
        outputType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
        outputType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        outputType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, blockAlign);
        outputType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, rate * blockAlign);
        reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM),
                                    nullptr, outputType);
        outputType->Release();
    }

    UINT32 peak = 0;
    UINT64 frames = 0;
    for (;;)
    {
        DWORD flags = 0;
        IMFSample* sample = nullptr;
        hr = reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0,
                                nullptr, &flags, nullptr, &sample);
        if (FAILED(hr) || sample == nullptr)
        {
            if (sample != nullptr)
            {
                sample->Release();
            }
            break;
        }

        IMFMediaBuffer* buffer = nullptr;
        if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buffer)) && buffer != nullptr)
        {
            BYTE* data = nullptr;
            DWORD length = 0;
            if (SUCCEEDED(buffer->Lock(&data, nullptr, &length)) && data != nullptr)
            {
                const int16_t* samples = reinterpret_cast<const int16_t*>(data);
                const size_t count = length / sizeof(int16_t);
                for (size_t index = 0; index < count; ++index)
                {
                    const int value = samples[index] < 0 ? -samples[index] : samples[index];
                    if (value > static_cast<int>(peak))
                    {
                        peak = static_cast<UINT32>(value);
                    }
                }
                frames += length / blockAlign;
                buffer->Unlock();
            }
            buffer->Release();
        }
        sample->Release();

        if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0)
        {
            break;
        }
    }

    reader->Release();

    if (framesOut != nullptr)
    {
        *framesOut = frames;
    }
    return peak;
}

// Writes a 32-bit float test tone.  Used by the self test when the machine has
// no capture endpoint, so that the native resampling + summing path can still be
// verified (the tone is deliberately mono at 44.1 kHz while the capture route
// runs at the device mix format).
bool WriteSyntheticWav(const std::wstring& path, UINT32 sampleRate, UINT16 channels,
                       int seconds, float frequency)
{
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    format.nChannels = channels;
    format.nSamplesPerSec = sampleRate;
    format.wBitsPerSample = 32;
    format.nBlockAlign = static_cast<WORD>(channels * 4);
    format.nAvgBytesPerSec = sampleRate * format.nBlockAlign;
    format.cbSize = 0;

    WavWriter writer;
    if (!writer.Open(path, &format))
    {
        return false;
    }

    const UINT32 totalFrames = sampleRate * static_cast<UINT32>(seconds > 0 ? seconds : 1);
    const UINT32 chunkFrames = 1024;
    std::vector<float> buffer(static_cast<size_t>(chunkFrames) * channels);

    for (UINT32 frame = 0; frame < totalFrames; frame += chunkFrames)
    {
        const UINT32 count = std::min(chunkFrames, totalFrames - frame);
        for (UINT32 index = 0; index < count; ++index)
        {
            const float time = static_cast<float>(frame + index) / static_cast<float>(sampleRate);
            const float value = 0.25f * sinf(2.0f * 3.14159265f * frequency * time);
            for (UINT16 channel = 0; channel < channels; ++channel)
            {
                buffer[static_cast<size_t>(index) * channels + channel] = value;
            }
        }
        writer.Write(reinterpret_cast<const BYTE*>(buffer.data()),
                     count * channels * 4u);
    }

    writer.Close();
    return true;
}

int RunSelfTest(const std::wstring& outputDir, const std::wstring& logPath, int seconds)
{
    SelfTestContext context;
    if (!EnsureDirectory(outputDir))
    {
        return 2;
    }

    const size_t separator = logPath.find_last_of(L"\\/");
    if (separator != std::wstring::npos)
    {
        EnsureDirectory(logPath.substr(0, separator));
    }

    _wfopen_s(&context.log, logPath.c_str(), L"w, ccs=UTF-8");
    context.Log(L"=== SimpleRecorder self test ===");

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr))
    {
        context.Log(L"CoInitializeEx (selftest) failed");
    }
    const bool mfOk = mfaudio::Startup();
    context.Log(std::wstring(L"MFStartup: ") + (mfOk ? L"ok" : L"failed"));

    bool writerOk = RunWriterSelfTest(context, outputDir);

    // hidden window for MFPlay
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = SelfTestWindowProc;
    windowClass.hInstance = g_instance;
    windowClass.lpszClassName = L"SimpleRecorderSelfTestWindow";
    RegisterClassExW(&windowClass);
    context.window = CreateWindowExW(0, windowClass.lpszClassName, L"selftest",
                                     WS_OVERLAPPED, 0, 0, 320, 200, nullptr, nullptr,
                                     g_instance, nullptr);

    if (context.window == nullptr)
    {
        // Without a window MFPlay would have no message target (and a NULL HWND
        // would broadcast the completion message), so stop here.
        context.Log(L"无法创建自检窗口，自检中止");
        UnregisterClassW(L"SimpleRecorderSelfTestWindow", g_instance);
        mfaudio::Shutdown();
        CoUninitialize();
        if (context.log != nullptr)
        {
            fclose(context.log);
        }
        return 2;
    }

    SetWindowLongPtrW(context.window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&context));

    int exitCode = 1;

    const std::wstring systemWav = outputDir + L"\\system.wav";
    const std::wstring micWav = outputDir + L"\\microphone.wav";
    const std::wstring systemM4a = outputDir + L"\\system_only.m4a";
    const std::wstring micM4a = outputDir + L"\\mic_only.m4a";
    const std::wstring mixedM4a = outputDir + L"\\mixed.m4a";

    DeleteFileW(systemWav.c_str());
    DeleteFileW(micWav.c_str());
    DeleteFileW(systemM4a.c_str());
    DeleteFileW(micM4a.c_str());
    DeleteFileW(mixedM4a.c_str());

    AudioCaptureEngine systemEngine;
    AudioCaptureEngine micEngine;

    std::wstring error;
    const bool systemOpened = systemEngine.Open(CaptureRoute::SystemLoopback, systemWav, error);
    context.Log(std::wstring(L"系统声音采集初始化: ") + (systemOpened ? L"ok" : L"failed") +
                L" - " + systemEngine.DescribeFormat() + L" " + error);
    error.clear();

    const bool micOpened = micEngine.Open(CaptureRoute::Microphone, micWav, error);
    context.Log(std::wstring(L"麦克风采集初始化: ") + (micOpened ? L"ok" : L"failed") +
                L" - " + micEngine.DescribeFormat() + L" " + error);
    error.clear();

    bool systemReady = false;
    bool micReady = false;

    if (systemOpened)
    {
        systemReady = systemEngine.Start(error);
        context.Log(std::wstring(L"启动系统声音采集: ") + (systemReady ? L"ok" : L"failed") +
                    L" " + error);
        error.clear();
    }

    if (micOpened)
    {
        micReady = micEngine.Start(error);
        context.Log(std::wstring(L"启动麦克风采集: ") + (micReady ? L"ok" : L"failed") +
                    L" " + error);
        error.clear();
    }

    if (systemReady || micReady)
    {
            // pump messages while recording so that any COM callbacks can run
            const ULONGLONG until = GetTickCount64() + static_cast<ULONGLONG>(seconds) * 1000;            while (GetTickCount64() < until)
            {
                MSG message{};
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
                {
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
                Sleep(50);
            }

            if (systemReady)
            {
                systemEngine.Stop();
            }
            if (micReady)
            {
                micEngine.Stop();
            }

            if (systemReady)
            {
                context.Log(L"系统声音: 采集帧数 " + std::to_wstring(systemEngine.FramesCaptured()) +
                            L", 存储格式 " + std::to_wstring(systemEngine.StoredFormat()->nSamplesPerSec) +
                            L" Hz/" + std::to_wstring(systemEngine.StoredFormat()->nChannels) + L" ch" +
                            L", 文件 " + std::to_wstring(FileSize(systemWav)) + L" 字节" +
                            L", 峰值 " + std::to_wstring(PeakLevel(systemWav)));
            }

            // Second route: the real microphone when present, otherwise a
            // synthetic 44.1 kHz mono tone inserted 200 ms after the start of
            // the other route (exercises resampling, channel conversion and the
            // QPC based alignment).
            bool secondRouteReady = micReady;
            unsigned long long secondQpc = micReady ? micEngine.BaseQpc()
                                                    : systemEngine.BaseQpc() + 2000000ULL;

            if (micReady)
            {
                context.Log(L"麦克风: 采集帧数 " + std::to_wstring(micEngine.FramesCaptured()) +
                            L", 存储格式 " + std::to_wstring(micEngine.StoredFormat()->nSamplesPerSec) +
                            L" Hz/" + std::to_wstring(micEngine.StoredFormat()->nChannels) + L" ch" +
                            L", 文件 " + std::to_wstring(FileSize(micWav)) + L" 字节" +
                            L", 峰值 " + std::to_wstring(PeakLevel(micWav)));
            }
            else
            {
                secondRouteReady = WriteSyntheticWav(micWav, 44100, 1, seconds, 440.0f);
                context.Log(std::wstring(L"本机无输入设备，生成合成第二路（44100 Hz / 单声道 / 正弦波）: ") +
                            (secondRouteReady ? L"ok" : L"failed") +
                            L", 文件 " + std::to_wstring(FileSize(micWav)) + L" 字节");
            }

            context.Log(L"两路起始时间差: " +
                        std::to_wstring(systemReady && micReady
                                            ? (micEngine.BaseQpc() > systemEngine.BaseQpc()
                                                   ? micEngine.BaseQpc() - systemEngine.BaseQpc()
                                                   : systemEngine.BaseQpc() - micEngine.BaseQpc())
                                            : 2000000ULL) +
                        L" (100ns)");

            bool allGood = true;
            int attempts = 0;

            auto encode = [&](const std::wstring& label,
                              const std::vector<mfaudio::InputFile>& inputs,
                              const std::wstring& output) {
                mfaudio::MixResult mixResult;
                std::wstring localError;
                const bool ok = mfaudio::MixToM4a(inputs, output, kAacBitrate, localError, &mixResult);
                ++attempts;
                const UINT64 bytes = FileSize(output);

                // Decode the result again: a file with the right duration can
                // still be silent, so verify the signal survived.
                UINT64 decodedFrames = 0;
                const UINT32 peak = ok ? DecodedPeakLevel(output, &decodedFrames) : 0;

                // Signal is only expected if a source actually contained some.
                UINT32 sourcePeak = 0;
                for (const mfaudio::InputFile& input : inputs)
                {
                    sourcePeak = std::max(sourcePeak, PeakLevel(input.path));
                }

                context.Log(label + (ok ? L": ok" : L": failed") + L" (" +
                            std::to_wstring(mixResult.durationMs) + L" ms, " +
                            std::to_wstring(mixResult.sampleRate) + L" Hz, " +
                            std::to_wstring(mixResult.channels) + L" ch, " +
                            std::to_wstring(mixResult.inputFrames) + L" frames, " +
                            std::to_wstring(bytes) + L" 字节, 解码后 " +
                            std::to_wstring(decodedFrames) + L" frames, 峰值 " +
                            std::to_wstring(peak) + L" (源峰值 " +
                            std::to_wstring(sourcePeak) + L"))" +
                            (localError.empty() ? std::wstring() : (L"  错误: " + localError)));

                // When a source carried a signal, the encoded file must carry it
                // too - a silent output is the failure this check exists for.
                const bool signalPreserved = sourcePeak <= 200 || peak > 200;
                const bool durationMatches =
                    decodedFrames * 1000 /
                            (mixResult.sampleRate ? mixResult.sampleRate : 1) +
                        500 >=
                    mixResult.durationMs;
                allGood = allGood && ok && bytes > 1000 && signalPreserved && durationMatches;
            };

            if (systemReady)
            {
                encode(L"系统声音 -> m4a", {{systemWav, systemEngine.BaseQpc()}}, systemM4a);
            }

            if (secondRouteReady)
            {
                encode(micReady ? L"麦克风 -> m4a" : L"合成第二路 -> m4a",
                       {{micWav, secondQpc}}, micM4a);
            }

            std::vector<mfaudio::InputFile> mixedInputs;
            if (systemReady)
            {
                mixedInputs.push_back({systemWav, systemEngine.BaseQpc()});
            }
            if (secondRouteReady)
            {
                mixedInputs.push_back({micWav, secondQpc});
            }

            const std::wstring mixedLabel = (systemReady && secondRouteReady)
                                                ? L"混合（两路对齐后合成） -> m4a"
                                                : L"单路（仅一路可用） -> m4a";
            encode(mixedLabel, mixedInputs, mixedM4a);

            // --- playback -----------------------------------------------------
            MediaPlayer player(context.window);
            bool ok = player.PlayFile(mixedM4a, error);
            context.Log(std::wstring(L"回放 (MFPlay): ") + (ok ? L"ok" : L"failed") + L" " + error);
            error.clear();

            if (ok)
            {
                const ULONGLONG playbackDeadline = GetTickCount64() +
                                                   static_cast<ULONGLONG>(seconds) * 1000 + 5000;
                while (!context.playbackEnded && GetTickCount64() < playbackDeadline)
                {
                    MSG message{};
                    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
                    {
                        TranslateMessage(&message);
                        DispatchMessageW(&message);
                    }
                    Sleep(20);
                }
                context.Log(std::wstring(L"回放结束事件: ") +
                            (context.playbackEnded ? L"收到" : L"超时未收到"));
                player.Shutdown();
            }

            allGood = allGood && attempts > 0 && context.playbackEnded && writerOk;
            context.Log(allGood ? L"结果: PASS" : L"结果: FAIL");
            exitCode = allGood ? 0 : 1;
    }
    else if (!systemOpened && !micOpened)
    {
        // Headless machines / CI runners have no audio endpoints.  The capture,
        // mixing and playback parts cannot be exercised, but the writer tests
        // above still ran, so report a distinct "skipped" code instead of a
        // failure (0 = PASS, 2 = skipped, 1 = FAIL).
        context.Log(L"本机没有可用音频设备（默认输出/输入端点都不存在）：跳过采集、合成与回放部分");
        context.Log(writerOk ? L"结果: SKIPPED (no audio devices)"
                             : L"结果: FAIL");
        exitCode = writerOk ? 2 : 1;
    }
    else
    {
        context.Log(L"音频设备存在但无法启动采集：采集、合成与回放部分已跳过");
        context.Log(L"结果: FAIL");
        exitCode = 1;
    }

    if (context.window != nullptr)
    {
        DestroyWindow(context.window);
    }
    UnregisterClassW(L"SimpleRecorderSelfTestWindow", g_instance);

    // Release the WASAPI/COM objects while COM is still initialized: releasing
    // them afterwards (from the destructors at the end of this function) can
    // fault because the apartment is already gone.
    systemEngine.Close();
    micEngine.Close();

    mfaudio::Shutdown();
    CoUninitialize();

    if (context.log != nullptr)
    {
        fclose(context.log);
    }
    return exitCode;
}

int RunApplication(int showCommand)
{
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr))
    {
        MessageBoxW(nullptr, L"无法初始化 COM。", L"启动失败", MB_OK | MB_ICONERROR);
        return 1;
    }

    if (!mfaudio::Startup())
    {
        MessageBoxW(nullptr, L"无法初始化 Media Foundation，录音功能不可用。",
                    L"启动失败", MB_OK | MB_ICONERROR);
        CoUninitialize();
        return 1;
    }

    INITCOMMONCONTROLSEX commonControls{};
    commonControls.dwSize = sizeof(commonControls);
    commonControls.dwICC = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&commonControls);

    g_backgroundBrush = GetSysColorBrush(COLOR_BTNFACE);

    // Best effort: drop session folders left behind by earlier killed runs.
    CleanupStaleSessions();

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = g_instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = g_backgroundBrush;
    windowClass.lpszClassName = kWindowClass;
    windowClass.style = CS_HREDRAW | CS_VREDRAW;

    if (RegisterClassExW(&windowClass) == 0)
    {
        MessageBoxW(nullptr, L"无法注册窗口类。", L"启动失败", MB_OK | MB_ICONERROR);
        mfaudio::Shutdown();
        CoUninitialize();
        return 1;
    }

    // Rough initial estimate - WM_CREATE pins the client area to the exact
    // size the content needs (before the window becomes visible).
    const int systemDpi = static_cast<int>(QuerySystemDpi());
    RECT rectangle = {0, 0, MulDiv(kMinimumClientWidth, systemDpi, 96),
                      MulDiv(430, systemDpi, 96)};
    const DWORD style = WS_OVERLAPPEDWINDOW;
    AdjustWindowRectEx(&rectangle, style, FALSE, WS_EX_CONTROLPARENT);

    HWND window = CreateWindowExW(WS_EX_CONTROLPARENT, kWindowClass, kWindowTitle, style,
                                  CW_USEDEFAULT, CW_USEDEFAULT,
                                  rectangle.right - rectangle.left,
                                  rectangle.bottom - rectangle.top,
                                  nullptr, nullptr, g_instance, nullptr);
    if (window == nullptr)
    {
        MessageBoxW(nullptr, L"无法创建窗口。", L"启动失败", MB_OK | MB_ICONERROR);
        mfaudio::Shutdown();
        CoUninitialize();
        return 1;
    }

    // MFPlay keeps a reference to its callback, so the player object lives for
    // the whole process lifetime; WM_DESTROY shuts it down instead of deleting it.
    g_player = new MediaPlayer(window);

    ShowWindow(window, showCommand);
    UpdateWindow(window);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0)
    {
        if (!IsDialogMessageW(window, &message))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

    delete g_player;
    g_player = nullptr;

    // Make sure no capture object outlives COM/MF.
    ReleaseEngines();

    if (g_font != nullptr)
    {
        DeleteObject(g_font);
    }

    mfaudio::Shutdown();
    CoUninitialize();
    return static_cast<int>(message.wParam);
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand)
{
    g_instance = instance;

    int argumentCount = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);

    if (arguments != nullptr)
    {
        for (int index = 1; index < argumentCount; ++index)
        {
            if (_wcsicmp(arguments[index], L"--selftest") == 0)
            {
                const int seconds = (index + 1 < argumentCount) ? _wtoi(arguments[index + 1]) : 5;
                std::wstring outputDir = (index + 2 < argumentCount)
                                             ? arguments[index + 2]
                                             : (TempRoot() + L"\\selftest");
                std::wstring logPath = (index + 3 < argumentCount)
                                           ? arguments[index + 3]
                                           : (outputDir + L"\\selftest.log");
                const int result = RunSelfTest(outputDir, logPath, seconds > 0 ? seconds : 5);
                LocalFree(arguments);
                return result;
            }
        }
    }

    if (arguments != nullptr)
    {
        LocalFree(arguments);
    }

    return RunApplication(showCommand);
}
