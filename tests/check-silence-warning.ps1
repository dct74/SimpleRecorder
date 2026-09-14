Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class Silence {
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr c);
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr h);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowW(string c, string t);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
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
[Silence]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null

$root = Split-Path -Parent $PSScriptRoot
$p = Start-Process -FilePath (Join-Path $root "build\bin\SimpleRecorder.exe") -PassThru
for ($i=0; $i -lt 40 -and $p.MainWindowHandle -eq 0; $i++) { Start-Sleep -Milliseconds 250; $p.Refresh() }
$w = $p.MainWindowHandle
$rec = [Silence]::GetDlgItem($w, 1010); $play = [Silence]::GetDlgItem($w, 1011)
$status = [Silence]::GetDlgItem($w, 1020); $modeSystem = [Silence]::GetDlgItem($w, 1002)
[Silence]::Click($modeSystem); Start-Sleep -Milliseconds 300
[Silence]::Click($rec); Start-Sleep -Seconds 3; [Silence]::Click($rec)
for ($i=0; $i -lt 60; $i++) { Start-Sleep -Milliseconds 500; if ([Silence]::IsWindowEnabled($play)) { break } }
Write-Output "--- status after recording SILENT system audio ---"
[Silence]::Text($status)
$hasWarning = [Silence]::Text($status).Contains([char]0x26A0)
Write-Output ("silence warning present: {0}" -f $hasWarning)
[Silence]::PostMessageW($w, [Silence]::WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
Start-Sleep -Milliseconds 800; [Silence]::Answer($p.Id, 7); Start-Sleep -Milliseconds 800
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
Remove-Item -Recurse -Force "$env:TEMP\SimpleRecorder" -ErrorAction SilentlyContinue
