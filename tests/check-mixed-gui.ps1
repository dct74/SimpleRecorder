Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class Mixed {
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr c);
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr h);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
  [DllImport("kernel32.dll")] public static extern uint GetCurrentProcessId();
  public delegate bool EnumProc(IntPtr h, IntPtr p);
  public const uint BM_CLICK = 0x00F5, WM_COMMAND = 0x0111, WM_CLOSE = 0x0010;
  public static string Text(IntPtr h){ var s=new StringBuilder(4096); GetWindowTextW(h,s,4096); return s.ToString(); }
  public static string Cls(IntPtr h){ var s=new StringBuilder(128); GetClassNameW(h,s,128); return s.ToString(); }
  public static IntPtr DialogOf(uint pid) {
    IntPtr found = IntPtr.Zero;
    EnumWindows((h, l) => { uint p; GetWindowThreadProcessId(h, out p);
      if (p == pid && Cls(h) == "#32770") { found = h; return false; } return true; }, IntPtr.Zero);
    return found;
  }
  public static void Click(IntPtr h){ PostMessageW(h, BM_CLICK, IntPtr.Zero, IntPtr.Zero); }
  public static void Answer(uint pid, int id){ IntPtr d = DialogOf(pid); if (d != IntPtr.Zero) PostMessageW(d, WM_COMMAND, (IntPtr)id, IntPtr.Zero); }
}
"@
[Mixed]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null

$root = Split-Path -Parent $PSScriptRoot
Remove-Item -Recurse -Force "$env:TEMP\SimpleRecorder" -ErrorAction SilentlyContinue

# play something so the system route carries a signal as well
$player = New-Object System.Media.SoundPlayer "C:\Windows\Media\Alarm01.wav"
$player.PlayLooping()
Start-Sleep -Milliseconds 400

$p = Start-Process -FilePath (Join-Path $root "build\bin\SimpleRecorder.exe") -PassThru
for ($i=0; $i -lt 40 -and $p.MainWindowHandle -eq 0; $i++) { Start-Sleep -Milliseconds 250; $p.Refresh() }
$w = $p.MainWindowHandle
$rec = [Mixed]::GetDlgItem($w, 1010); $play = [Mixed]::GetDlgItem($w, 1011)
$status = [Mixed]::GetDlgItem($w, 1020); $modeMix = [Mixed]::GetDlgItem($w, 1004)

[Mixed]::Click($modeMix); Start-Sleep -Milliseconds 300
$started = Get-Date
[Mixed]::Click($rec)
Start-Sleep -Seconds 5
[Mixed]::Click($rec)
for ($i=0; $i -lt 60; $i++) { Start-Sleep -Milliseconds 500; if ([Mixed]::IsWindowEnabled($play)) { break } }
$elapsed = [Math]::Round(((Get-Date) - $started).TotalSeconds, 1)
$player.Stop()

Write-Output "=== GUI mixed recording (real mic + system audio) ==="
Write-Output ("wall clock of record+merge: {0}s" -f $elapsed)
Write-Output "--- status text ---"
$text = [Mixed]::Text($status)
$text
Write-Output ("contains a warning: {0}" -f $text.Contains([char]0x26A0))

$session = Get-ChildItem "$env:TEMP\SimpleRecorder" -Directory | Select-Object -First 1
$result = Join-Path $session.FullName "result.m4a"
$leftovers = (Get-ChildItem $session.FullName -File | Measure-Object).Count
Write-Output ("result file: {0} bytes, files left in session dir: {1}" -f (Get-Item $result).Length, $leftovers)
$shell = New-Object -ComObject Shell.Application
$folder = $shell.Namespace($session.FullName)
$item = $folder.ParseName("result.m4a")
Write-Output ("m4a duration (shell): {0}  bitrate: {1}" -f $folder.GetDetailsOf($item, 27), $folder.GetDetailsOf($item, 28))

Write-Output "--- playback through the UI ---"
[Mixed]::Click($play); Start-Sleep -Seconds 2
Write-Output ("play button now: '{0}'" -f [Mixed]::Text($play))
[Mixed]::Click($play); Start-Sleep -Milliseconds 500
Write-Output ("after stop:      '{0}'" -f [Mixed]::Text($play))

[Mixed]::PostMessageW($w, [Mixed]::WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
Start-Sleep -Milliseconds 800; [Mixed]::Answer($p.Id, 7); Start-Sleep -Milliseconds 800
Write-Output ("exited: {0}" -f $p.HasExited)
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
$left = (Get-ChildItem "$env:TEMP\SimpleRecorder" -Recurse -File -ErrorAction SilentlyContinue | Measure-Object).Count
Write-Output ("temp files after exit: {0}" -f $left)
Remove-Item -Recurse -Force "$env:TEMP\SimpleRecorder" -ErrorAction SilentlyContinue
