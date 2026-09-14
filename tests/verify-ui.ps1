Add-Type @"
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public class Ver {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
  public delegate bool EnumProc(IntPtr h, IntPtr p);
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr c);
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr h, EnumProc cb, IntPtr p);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr h);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr a, int x, int y, int cx, int cy, uint f);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)] public struct LOGFONT {
    public int lfHeight, lfWidth, lfEscapement, lfOrientation, lfWeight;
    public byte lfItalic, lfUnderline, lfStrikeOut, lfCharSet, lfOutPrecision, lfClipPrecision, lfQuality, lfPitchAndFamily;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst=32)] public string lfFaceName;
  }
  [DllImport("gdi32.dll", CharSet=CharSet.Unicode)] public static extern int GetObjectW(IntPtr obj, int size, ref LOGFONT lf);
  public const uint WM_GETFONT = 0x0031;
  public static LOGFONT FontOf(IntPtr ctl) {
    IntPtr font = SendMessageW(ctl, WM_GETFONT, IntPtr.Zero, IntPtr.Zero);
    LOGFONT lf = new LOGFONT();
    if (font != IntPtr.Zero) GetObjectW(font, Marshal.SizeOf(typeof(LOGFONT)), ref lf);
    return lf;
  }
  public static string FontInfo(IntPtr ctl) {
    LOGFONT lf = FontOf(ctl);
    return "face='" + lf.lfFaceName + "' height=" + lf.lfHeight + " weight=" + lf.lfWeight;
  }
  public const uint BM_CLICK = 0x00F5, WM_COMMAND = 0x0111, WM_CLOSE = 0x0010;
  public static string Text(IntPtr h){ var s=new StringBuilder(4096); GetWindowTextW(h,s,4096); return s.ToString(); }
  public static string Cls(IntPtr h){ var s=new StringBuilder(128); GetClassNameW(h,s,128); return s.ToString(); }
  public static int W(IntPtr h){ RECT r; GetWindowRect(h, out r); return r.Right-r.Left; }
  public static int H(IntPtr h){ RECT r; GetWindowRect(h, out r); return r.Bottom-r.Top; }
  public static RECT RectOf(IntPtr h){ RECT r; GetWindowRect(h, out r); return r; }
  public static IntPtr DialogOfProcess(uint pid) {
    IntPtr found = IntPtr.Zero;
    EnumWindows((h, l) => {
      uint p; GetWindowThreadProcessId(h, out p);
      if (p == pid && Cls(h) == "#32770") { found = h; return false; }
      return true;
    }, IntPtr.Zero);
    return found;
  }
  public static void Click(IntPtr h){ PostMessageW(h, BM_CLICK, IntPtr.Zero, IntPtr.Zero); }
  public static void Close(IntPtr h){ PostMessageW(h, WM_CLOSE, IntPtr.Zero, IntPtr.Zero); }
  public static void Resize(IntPtr h,int w,int hh){ SetWindowPos(h, IntPtr.Zero, 0,0,w,hh, 0x0004|0x0010|0x0002); }
  public static void AnswerDialog(uint pid, int id){ IntPtr d = DialogOfProcess(pid); if (d != IntPtr.Zero) PostMessageW(d, WM_COMMAND, (IntPtr)id, IntPtr.Zero); }
}
"@
[Ver]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null

$root = Split-Path -Parent $PSScriptRoot   # SimpleRecorder/
$exePath = Join-Path $root "build\bin\SimpleRecorder.exe"
if (-not (Test-Path $exePath)) {
  Write-Output "Build first: package\SimpleRecorder.exe -> $exePath missing"
  exit 2
}
$trace = Join-Path $root "build\verify-trace.txt"
Remove-Item $trace -ErrorAction SilentlyContinue
Remove-Item -Recurse -Force "$env:TEMP\SimpleRecorder" -ErrorAction SilentlyContinue
$env:SIMPLERECORDER_TRACE = $trace
$p = Start-Process -FilePath $exePath -PassThru
for ($i=0; $i -lt 40 -and $p.MainWindowHandle -eq 0; $i++) { Start-Sleep -Milliseconds 250; $p.Refresh() }
$w = $p.MainWindowHandle
Start-Sleep -Milliseconds 500

# --- the layout numbers the application measured for itself -------------------
$layout = (Get-Content $trace -Encoding UTF8 | Where-Object { $_ -match '^layout:' } | Select-Object -Last 1)
Write-Output "app trace: $layout"
$radioText = [int]([regex]::Match($layout, 'radioText=(\d+)').Groups[1].Value)
$hintRes  = [int]([regex]::Match($layout, 'hintReserve=(\d+)').Groups[1].Value)
$statRes  = [int]([regex]::Match($layout, 'statusReserve=(\d+)').Groups[1].Value)
$minW     = [int]([regex]::Match($layout, 'min=(\d+)x').Groups[1].Value)
$minH     = [int]([regex]::Match($layout, 'min=\d+x(\d+)').Groups[1].Value)

$ids = @{ 1001='group'; 1002='radio1'; 1003='radio2'; 1004='radio3'; 1010='record'; 1011='play'; 1012='save'; 1020='status'; 1021='hint' }
$fail = 0
function Check($label, $ok, $detail) {
  if (-not $ok) { $script:fail++ }
  Write-Output ("  [{0}] {1} - {2}" -f $(if($ok){'PASS'}else{'FAIL'}), $label, $detail)
}

Write-Output "== 1. window / resize =="
$rec = [Ver]::GetDlgItem($w, 1010); $play = [Ver]::GetDlgItem($w, 1011); $save = [Ver]::GetDlgItem($w, 1012)
$cw = [Ver]::W($w); $ch = [Ver]::H($w)
Check "initial size >= minimum" ($cw -ge $minW) "window ${cw}x${ch}, app minimum ${minW}x${minH}"
foreach ($id in 1002,1003,1004) {
  $ctl = [Ver]::GetDlgItem($w, $id)
  $rw = [Ver]::W($ctl)
  Check ("at initial size: radio [{0}] wide enough" -f $ids[$id]) ($rw -ge ($radioText + 30)) "control=${rw}px, text=${radioText}px, needed>=$($radioText+30)"
}
$hint0 = [Ver]::GetDlgItem($w, 1021)
Check "at initial size: hint tall enough" ([Ver]::H($hint0) -ge $hintRes) "control=$([Ver]::H($hint0))px >= ${hintRes}px"
[Ver]::Resize($w, 200, 150); Start-Sleep -Milliseconds 500
$smallW = [Ver]::W($w); $smallH = [Ver]::H($w)
Check "shrinking is clamped by WM_GETMINMAXINFO" ($smallW -ge $minW -and $smallH -ge $minH) "got ${smallW}x${smallH}"
foreach ($id in 1002,1003,1004) {
  $ctl = [Ver]::GetDlgItem($w, $id)
  Check ("at minimum size: radio [{0}] wide enough" -f $ids[$id]) ([Ver]::W($ctl) -ge ($radioText + 30)) "control=$([Ver]::W($ctl))px"
}
Check "at minimum size: hint tall enough" ([Ver]::H($hint0) -ge $hintRes) "control=$([Ver]::H($hint0))px"
[Ver]::Resize($w, 900, 700); Start-Sleep -Milliseconds 500
Check "growing works" ([Ver]::W($w) -ge 900) ("got {0}x{1}" -f [Ver]::W($w), [Ver]::H($w))

# --- all three buttons must look exactly the same --------------------------
$fRec = [Ver]::FontOf($rec); $fPlay = [Ver]::FontOf($play); $fSave = [Ver]::FontOf($save)
$sameFont = ($fRec.lfHeight -eq $fPlay.lfHeight) -and ($fPlay.lfHeight -eq $fSave.lfHeight) -and
            ($fRec.lfWeight -eq $fPlay.lfWeight) -and ($fPlay.lfWeight -eq $fSave.lfWeight) -and
            ($fRec.lfFaceName -eq $fPlay.lfFaceName) -and ($fPlay.lfFaceName -eq $fSave.lfFaceName)
Check "three buttons use the same font" $sameFont "record=[$([Ver]::FontInfo($rec))] play=[$([Ver]::FontInfo($play))] save=[$([Ver]::FontInfo($save))]"
$fRadio = [Ver]::FontOf([Ver]::GetDlgItem($w, 1002))
$fStatus = [Ver]::FontOf([Ver]::GetDlgItem($w, 1020))
Check "button font matches the rest of the UI" (($fRec.lfHeight -eq $fRadio.lfHeight) -and ($fRec.lfHeight -eq $fStatus.lfHeight) -and ($fRec.lfWeight -eq 400)) "button height=$($fRec.lfHeight) radio=$($fRadio.lfHeight) status=$($fStatus.lfHeight) weight=$($fRec.lfWeight)"

$btnW = @([Ver]::W($rec), [Ver]::W($play), [Ver]::W($save))
$btnH = @([Ver]::H($rec), [Ver]::H($play), [Ver]::H($save))
Check "three buttons have identical size" (($btnW | Select-Object -Unique).Count -eq 1 -and ($btnH | Select-Object -Unique).Count -eq 1) "widths=$($btnW -join ',') heights=$($btnH -join ',')"
[Ver]::Resize($w, 200, 150); Start-Sleep -Milliseconds 500
$btnW2 = @([Ver]::W($rec), [Ver]::W($play), [Ver]::W($save))
Check "buttons stay identical at minimum size" (($btnW2 | Select-Object -Unique).Count -eq 1) "widths=$($btnW2 -join ',') on a $([Ver]::W($w))px window"

Write-Output "== 2. every control fits inside the client area, no overlap =="
$client = [Ver]::RectOf($w)   # window rect; controls must be inside (client is smaller)
$controls = @()
[Ver]::EnumChildWindows($w, { param($h, $l) $script:controls += $h; return $true }, [IntPtr]::Zero) | Out-Null
$rects = @{}
foreach ($h in $controls) { $rects[$h] = [Ver]::RectOf($h) }
$inside = $true
foreach ($h in $controls) {
  $r = $rects[$h]
  if ($r.Right -gt $client.Right -or $r.Bottom -gt $client.Bottom) { $inside = $false }
  if (-not [Ver]::IsWindowVisible($h)) { $inside = $false }
}
Check "all controls visible and inside the window" $inside ("{0} controls" -f $controls.Count)
foreach ($id in 1002,1003,1004) {
  $ctl = [Ver]::GetDlgItem($w, $id)
  $rw = [Ver]::W($ctl)
  Check ("radio [{0}] wide enough" -f $ids[$id]) ($rw -ge ($radioText + 30)) "control=${rw}px, text=${radioText}px, needed>=$($radioText+30)"
}
$hint = [Ver]::GetDlgItem($w, 1021); $status = [Ver]::GetDlgItem($w, 1020)
Check "hint box tall enough" ([Ver]::H($hint) -ge $hintRes) "control=$([Ver]::H($hint))px >= reserve=${hintRes}px"
Check "status box >= worst case reserve" ([Ver]::H($status) -ge $statRes) "control=$([Ver]::H($status))px >= reserve=${statRes}px"

Write-Output "== 3. state machine =="
$r1 = [Ver]::GetDlgItem($w, 1002)
Check "idle: record enabled, play/save disabled" ([Ver]::IsWindowEnabled($rec) -and -not [Ver]::IsWindowEnabled($play) -and -not [Ver]::IsWindowEnabled($save)) "record=$([Ver]::IsWindowEnabled($rec)) play=$([Ver]::IsWindowEnabled($play)) save=$([Ver]::IsWindowEnabled($save))"

[Ver]::Click($r1); Start-Sleep -Milliseconds 300
[Ver]::Click($rec); Start-Sleep -Seconds 3
Check "recording: mode radios disabled" (-not [Ver]::IsWindowEnabled($r1)) "record button text len=$([Ver]::Text($rec).Length)"
[Ver]::Click($rec)
for ($i=0; $i -lt 60; $i++) { Start-Sleep -Milliseconds 500; if ([Ver]::IsWindowEnabled($play)) { break } }
Check "after stop: play+save enabled" ([Ver]::IsWindowEnabled($play) -and [Ver]::IsWindowEnabled($save)) "play=$([Ver]::IsWindowEnabled($play)) save=$([Ver]::IsWindowEnabled($save))"
Check "after stop: record button enabled again" ([Ver]::IsWindowEnabled($rec)) "record=$([Ver]::IsWindowEnabled($rec))"
Check "after stop: mode radios enabled again" (([Ver]::IsWindowEnabled($r1)) -and ([Ver]::IsWindowEnabled([Ver]::GetDlgItem($w,1003))) -and ([Ver]::IsWindowEnabled([Ver]::GetDlgItem($w,1004)))) "all three selectable"

Write-Output "== 4. second recording without restarting =="
[Ver]::Click($rec); Start-Sleep -Milliseconds 900
[Ver]::AnswerDialog($p.Id, 6); Start-Sleep -Milliseconds 900
$started = [Ver]::IsWindowEnabled($play) -eq $false
Check "new recording actually started" $started "play became disabled => capturing (play=$([Ver]::IsWindowEnabled($play)))"
if ($started) {
  Start-Sleep -Seconds 2; [Ver]::Click($rec)
  for ($i=0; $i -lt 60; $i++) { Start-Sleep -Milliseconds 500; if ([Ver]::IsWindowEnabled($play)) { break } }
  Check "second recording finished" ([Ver]::IsWindowEnabled($play)) "play enabled again"
} else {
  [Ver]::AnswerDialog($p.Id, 7)
}

Write-Output "== 5. save dialog and clean exit =="
foreach ($id in 1002,1003,1004) { }
[Ver]::Click($save); Start-Sleep -Milliseconds 1500
$dlg = [Ver]::DialogOfProcess($p.Id)
Check "save dialog opens" ($dlg -ne [IntPtr]::Zero) "dialog handle=$dlg"
if ($dlg -ne [IntPtr]::Zero) {
  [Ver]::Close($dlg); Start-Sleep -Milliseconds 900
  Check "cancelling the save dialog returns to the app" (-not $p.HasExited -and [Ver]::IsWindowEnabled($play)) "exited=$($p.HasExited) play=$([Ver]::IsWindowEnabled($play))"
}
[Ver]::Close($w); Start-Sleep -Milliseconds 900
$prompt = [Ver]::DialogOfProcess($p.Id)
Check "unsaved-recording prompt appears" ($prompt -ne [IntPtr]::Zero) "dialog handle=$prompt"
[Ver]::AnswerDialog($p.Id, 7); Start-Sleep -Milliseconds 900   # 7 = No -> discard and exit
Check "process exited" $p.HasExited "exited=$($p.HasExited)"
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
$left = Get-ChildItem "$env:TEMP\SimpleRecorder" -Recurse -File -ErrorAction SilentlyContinue | Measure-Object
Check "temporary files cleaned up after discard" ($left.Count -eq 0) "$($left.Count) file(s) left"
Remove-Item Env:\SIMPLERECORDER_TRACE -ErrorAction SilentlyContinue

Write-Output ""
Write-Output ("RESULT: {0} failure(s)" -f $fail)
