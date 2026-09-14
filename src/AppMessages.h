#pragma once

#include <windows.h>

// Application messages
#define WM_APP_PROCESS_DONE     (WM_APP + 11) // encoding/mixing finished
#define WM_APP_PLAYBACK_ENDED   (WM_APP + 12) // playback finished or failed

// Control IDs
#define IDC_GROUP_MODE          1001
#define IDC_MODE_SYSTEM         1002
#define IDC_MODE_MIC            1003
#define IDC_MODE_MIX            1004
#define IDC_RECORD_BTN          1010
#define IDC_PLAY_BTN            1011
#define IDC_SAVE_BTN            1012
#define IDC_STATUS_TEXT         1020
#define IDC_HINT_TEXT           1021

// Timer IDs
#define IDT_ELAPSED             1
