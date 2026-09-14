# SimpleRecorder — 极简原生 Windows 录音工具（M4A）

参考上游 `AudioCapture` 项目的采集思路，独立实现的一个**只做三件事**的录音工具，输出统一为 **.m4a**（AAC）。
代码为本仓库原创（MIT 许可），无任何第三方依赖。

> **下载即用**：[Releases](https://github.com/dct74/SimpleRecorder/releases) 里下载 `SimpleRecorder.exe`（单文件、约 380 KB），
> 拷到任意 Windows 10/11 x64 机器双击即可，无需安装、无需运行时。也可以自行编译（见第 4 节）。

- 录制系统声音（单独）
- 录制麦克风声音（单独）
- 混合录制：系统声音与麦克风**分两路独立采集**，停止后再用 **Windows 原生 Media Foundation 管线**对齐、合成、编码为一个 m4a

界面只有：**三个模式单选**、**录制/停止（单按钮切换）**、**回放**、**保存为 M4A**。
没有任何第三方库（不用 ffmpeg、不用 opus/flac/nlohmann-json），全部依赖操作系统组件，单文件 exe，无需安装。

---

## 1. 界面

```
┌─ 录制模式 ─────────────────────────────────────────────┐
│  ① 只录制系统声音（电脑正在播放的声音）                │
│  ② 只录制麦克风声音                                    │
│  ③ 混合录制（系统声音 + 麦克风，停止后合成为一个文件）  │
└────────────────────────────────────────────────────────┘
  [ ●  开始录制 ]   [ ▶ 回放 ]   [ 保存为 M4A… ]

  状态区（录制中显示已录制时长；完成后显示时长与文件大小）
  提示区（输出格式说明）
```

按钮语义：

| 控件 | 行为 |
| --- | --- |
| `● 开始录制` | 开始录制（按所选模式启动 1 路或 2 路采集），按钮变为 `■ 停止录制` |
| `■ 停止录制` | 停止采集 → 后台合成 m4a → 完成后 `回放/保存` 按钮可用 |
| `▶ 回放` / `■ 停止回放` | 用系统播放器（MFPlay）试听合成结果，可中途停止 |
| `保存为 M4A…` | 弹出系统“另存为”对话框，把临时 m4a 复制到目标位置 |

窗口与状态规则：

- **三个按钮（录制/停止、回放、保存）外观完全一致**：等宽、等高、同一字体同一字号（使用系统消息字体，按显示器 DPI 缩放，不做加粗/放大），平分可用宽度，窗口缩放时同步变化。
- **窗口可自由缩放**（拖边框、最大化）。布局按内容自动重排：状态区吸收多余高度；
  最小尺寸由实际字体量测得出（不是写死的），小于最小尺寸时系统会阻止继续缩小，保证文字永不被裁切。
- **录音结束后可以直接再录下一段**：`开始录制` 按钮和三个模式选项始终可用（只有合成那几秒会暂时禁用）。
  若上一段录音尚未保存，会先弹窗确认再丢弃。
- 未保存的录音会一直在状态区显示 **临时文件路径**，并以 `（退出时删除）` 标明它的命运。
- 关闭窗口时若录音尚未保存，弹出三选项提示：
  “是”= 现在保存为 M4A 文件； “否”= 丢弃并退出（删除临时文件）； “取消”= 返回程序。
- 界面随显示器 DPI 缩放（Per-Monitor V2），在 125%/150% 等缩放比例下字体与控件同步放大。

---

## 2. 底层原理

### 2.1 三种模式各自的采集路径（WASAPI）

| 模式 | 采集对象 | WASAPI 方式 |
| --- | --- | --- |
| 系统声音 | 默认**输出**端点（扬声器/耳机） | `IMMDeviceEnumerator::GetDefaultAudioEndpoint(eRender, eConsole)` + `IAudioClient::Initialize` 带 `AUDCLNT_STREAMFLAGS_LOOPBACK`，共享模式，使用 `GetMixFormat()` 的设备混音格式（通常 48 kHz / 2ch / 32-bit float） |
| 麦克风 | 默认**输入**端点（麦克风/线路输入） | `GetDefaultAudioEndpoint(eCapture, eConsole)`，同一个 `IAudioClient`，**不带** LOOPBACK（正常采集流） |
| 混合 | 上面两路**同时**各自独立运行 | 两个 `AudioCaptureEngine` 实例，各自一个采集线程，各自一个临时文件 |

采集循环（每路一个线程，`AudioCaptureEngine::CaptureThread`）：

```
AvSetMmThreadCharacteristics(L"Audio")          // 音频线程优先级
while (running)
    Sleep(10)
    GetNextPacketSize / GetBuffer / ReleaseBuffer   // 标准 WASAPI 拉取
```

### 2.2 关键点：让两路"时间轴对齐"

混合录制必须解决两个现实问题：

1. **loopback 在没有声音时不送包。** 系统处于静音（没人播放）时，回环采集端点长时间不给数据；如果"来多少写多少"，录到的文件会丢掉这些静音时段，导致与麦克风轨错位。
2. **两路不可能同时启动。** 第二次 `Start()` 必然比第一次晚几毫秒到几十毫秒。

做法（`AudioCaptureEngine`）：

- `Start()` 成功后立刻取一次 `QueryPerformanceCounter` 作为 **这条轨道第 0 帧的绝对时间** `BaseQpc`（100 ns 单位）。
- 每个数据包都带 `GetBuffer(..., &qpc)` 给出的该包首帧时间戳。若 `qpc` 比"预期时间"晚，就把差值补成**静音帧**再写数据 → 时间轴永远连续。
- `Stop()` 时再按 `now - BaseQpc` 补齐尾部静音，即使这一路**一个包都没收到**（整段系统静音），文件时长依然等于真实录制时长。
- 于是两条 wav 各自携带一个绝对起始时间，合成时按 `BaseQpc` 差值插入前导静音即可对齐。

采集线程把数据交给 `WavWriter`：**临时文件统一存 16-bit PCM**（32-bit float 混音格式会转换为 16-bit），一小时 48 kHz 立体声约 350 MB，而 float32 要 1.3 GB。

#### 安全阀：时间轴永远不会超前于真实时间

设备返回的 QPC 时间戳（100 ns 单位）在少数驱动/主板上会**跳变、漂移或基于不同的时间基准**。若无条件相信它，一次异常跳变就会被理解成“这段设备没送数据，都是静音”而写进文件：
几秒的录音会变成几分钟甚至更长的文件，合成时还要把这些静音一起编码 —— 表现就是“只录了几秒钟，却显示五分钟、合并等很久”。

因此 `AudioCaptureEngine` 维持一个硬不变量：

```
已写入时间轴帧数  ≤  真实经过时间（用自己的 QueryPerformanceCounter 测量）× 采样率 + 2 秒余量
```

任何超出这个上限的“静音”都会被拒绝（只写到上限为止），并记入 `DroppedFrames()`。发生这种情况时，完成状态会多一行提示：

```
⚠ 录音设备返回了异常时间戳，已按真实录制时长忽略 X.X 秒静音
```

也就是说：**录音长度始终等于你实际按下录音到停止的那段时间**；设备时间戳只能决定“中间哪里是静音”，不能决定文件有多长。

### 2.3 混合合成：Windows 原生管线

停止录制后，`mfaudio::MixToM4a()` 在后台线程里完成（`src/MfAudio.cpp`）：

```
  system.wav ─┐
              ├─ IMFSourceReader（原生 WAV 解析 / 解码）
  microphone.wav ─┘        │
                           ├─ 原生 Audio Resampler DSP（采样率 / 声道数转换，
                           │  例如 44.1 kHz 单声道 → 48 kHz 立体声）
                           ↓
              统一为 PCM 16-bit / 48 kHz / 2ch（单路时保留原始采样率与声道）
                           ↓
              按 BaseQpc 差补前导静音 → 逐帧相加（int32 累加 + 限幅）
                           ↓
              IMF SinkWriter ─→ 原生 AAC 编码器 MFT（AAC-LC, 192 kbps）
                             └→ 原生 MPEG-4 媒体池（MFCreateMPEG4MediaSink）
                           ↓
                       result.m4a
```

具体使用的系统组件：

| 环节 | 组件 |
| --- | --- |
| 读取临时 WAV | `MFCreateSourceReaderFromURL`（WAVE 字节流处理器） |
| 采样率/声道转换 | 源读取器自动插入的 **Audio Resampler DSP**（`MF_MT_SUBTYPE = MFAudioFormat_PCM` 输出类型触发） |
| 编码 | 系统 **AAC 编码器 MFT**（`MFAudioFormat_AAC`，`MF_MT_AAC_PAYLOAD_TYPE = 0`） |
| 封装 | `MFCreateMPEG4MediaSink` + `MFCreateSinkWriterFromMediaSink` → `.m4a` |
| 回放 | **MFPlay**（`MFPCreateMediaPlayer`），失败时退回 `ShellExecute` 用系统默认播放器 |

> **关于"用系统原生管线合成"的说明**
> 解码、重采样、声道转换、AAC 编码、MP4 封装全部是 Windows 自带组件，程序里没有自带任何编解码器。
> 唯一由本程序完成的一步是**逐帧相加**（float/int32 累加后限幅）。Windows 并没有提供"离线把两个音频文件混合成一个"的现成组件：
> 系统里唯一的混音器是**音频引擎实时混音**（需要把两路实时渲染到设备再回环采集，会出声、会串入其它系统声音、且依赖播放设备），
> 而 Media Foundation 只有解码、重采样、编码类 MFT，没有 mix MFT。因此"两路 PCM 相加"必须由程序完成，这也是最精确、无额外延迟的做法。

### 2.4 为什么"先存临时文件、停止后再转码"

- 两条轨道可以各自独立、互不阻塞地落盘，停止前不需要实时混音线程，逻辑简单且不会因为一路慢而丢数据；
- 停止后可以**离线**用最好的质量做重采样/编码（不受实时性约束）；
- 单路模式也走同一条管线，只有一份代码路径；
- 停止时按钮变为"正在合成…"，长录音需要几秒钟（约 100 倍实时速度，1 小时录音约 30 秒以内）。

### 2.5 文件位置

| 阶段 | 位置 |
| --- | --- |
| 临时采集/合成 | `%TEMP%\SimpleRecorder\rec-YYYYMMDD-HHMMSS\`（`system.wav` / `microphone.wav` / `result.m4a`） |
| 合成成功 | 临时 wav 删除，只留 `result.m4a`（可回放）；该路径会显示在状态区 |
| 点击“保存为 M4A…” | 复制到你选择的位置，默认文件名 `系统声音_/麦克风_/混合录音_YYYY-MM-DD_HH-MM-SS.m4a`，默认目录“音乐” |
| 开始新录音 | 上一次的临时目录整体删除 |
| **未保存就退出程序** | **弹窗三选一**：“是”→ 先保存；“否”→ 丢弃并删除临时文件；“取消”→ 不退出 |

也就是说：**只要没有点“保存为 M4A…”，录音就只存在于系统临时目录里，退出程序时会被删除**
（提示里选“是”可以先保存；选“取消”则不会退出，录音仍在，可继续回放/保存）。
如果需要保证不丢，记得录音结束后点一下“保存为 M4A…”，或者退出提示里选“是”。

%TEMP% 的典型位置：`C:\Users\<用户名>\AppData\Local\Temp\SimpleRecorder\`（在资源管理器地址栏输入 `%TEMP%` 即可打开）。

---

## 3. 代码结构

```
SimpleRecorder/
├─ CMakeLists.txt            # 只链接系统库，无第三方依赖
├─ build.bat                 # 一键构建（自动找 VS + cmake，产出 package\SimpleRecorder.exe）
├─ app.manifest              # Common Controls v6 + Per-Monitor-V2 DPI 感知
├─ app.rc
├─ tests/
│  └─ verify-ui.ps1          # 界面自动化回归测试（缩放/最小尺寸/控件区域/状态机/清理）
└─ src/
   ├─ main.cpp               # Win32 界面、DPI 缩放、自适布局、状态机、保存对话框、自检模式
   ├─ AudioCaptureEngine.*   # WASAPI 单路采集（回环 / 麦克风 + QPC 时间轴 + 静音补齐）
   ├─ WavWriter.*            # 临时 WAV 写入（float32/24/32/8 → PCM16 转换）
   ├─ MfAudio.*              # 源读取器解码 + 原生重采样 + 相加 + AAC/MP4 编码（核心）
   ├─ MediaPlayer.*          # MFPlay 回放封装
   └─ AppMessages.h          # 控件 ID / 自定义消息
```

---

## 4. 构建
需要：Visual Studio 2019+（含 C++ 桌面开发）、Windows SDK 10.0.19041+、CMake 3.15+。
**不需要 vcpkg，不需要任何第三方包。**

```cmd
cd SimpleRecorder
build.bat
```

或手工：

```cmd
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
:: 产物：build\bin\SimpleRecorder.exe
```

也可用 Visual Studio 生成器：`cmake -S . -B build -G "Visual Studio 18 2026" -A x64`。

## 5. 部署：单文件，免安装，无第三方运行时

`package\SimpleRecorder.exe` 只有一个文件（约 350 KB），**直接拷贝到其他电脑就能跑，不需要安装任何东西**：

- C/C++ 运行时**静态链接**（`MSVC_RUNTIME_LIBRARY = MultiThreaded`，即 `/MT`）：
  导入表里没有 `vcruntime140.dll` / `msvcp140.dll`，也没有 `api-ms-win-crt-*.dll`，**不需要安装 VC++ Redistributable**；
- 不依赖任何第三方 DLL（没有 ffmpeg / opus / flac / json 库）；
- 导入的 11 个 DLL 全部是 Windows 自带文件，已逐个确认存在于 `C:\Windows\System32`：

  ```
  AVRT.dll        COMCTL32.dll    MF.dll         MFPlat.dll      MFPlay.dll
  MFReadWrite.dll ole32.dll       SHELL32.dll    KERNEL32.dll    USER32.dll    GDI32.dll
  ```
- 不需要管理员权限，不写注册表，不生成配置文件（唯一写盘位置是 `%TEMP%\SimpleRecorder\`）。
- 按 `per-monitor v2` DPI 感知，带 Common Controls v6 清单，无外部资源文件依赖。

### 对目标机器的要求

| 项目 | 要求 |
| --- | --- |
| 体系结构 | x64（当前产物）。ARM64 版 Windows 11 靠 x64 仿真运行 |
| 操作系统 | Windows 7 SP1 及以上（实际目标：Windows 10 / 11）。程序对 `GetDpiForWindow` 等新 API 做了动态加载与回退 |
| 系统组件 | **Media Foundation**（`MFPlat.dll` / `MFReadWrite.dll` / `MFPlay.dll`）与 **WASAPI**（`MMDevAPI`）——均为标准 Windows 组件；AAC 编码器也是系统自带的 MFT |
| 例外 | Windows **N / KN 版**（欧盟/韩国）默认不含 Media Foundation，需安装微软免费的 *Media Feature Pack*；Windows 10/11 **S 模式**不允许运行非商店 exe；Server Core 等精简镜像同理 |

> 如果目标机器是 32 位 Windows，用 Visual Studio 生成器重新编一份 Win32 版本即可（源码与 CMakeLists 无需修改）：
> `cmake -S . -B build-x86 -G "Visual Studio 18 2026" -A Win32`，然后 `cmake --build build-x86 --config Release`。
> （已实测：x86 版本可编译、导入表同样只有系统 DLL，并且自检通过。）

### 自检模式（命令行）

用于在无人值守环境下验证"采集 → 转码 → 混合 → 回放"整条链路：

退出码：**0 = 全部通过**；**2 = 写入器自检通过，但本机没有音频端点（采集/合成/回放跳过）**；**1 = 失败**。
因此它可以在无音频设备的机器（包括 CI runner）上运行，至少验证格式转换与编码链路。

```cmd
SimpleRecorder.exe --selftest <秒数> <输出目录> <日志文件>
```

它会：录制系统声音若干秒；有麦克风就用真实麦克风，没有就生成一路 44.1 kHz 单声道的合成信号（用于验证原生重采样与对齐）；分别导出单路 m4a 与混合 m4a；用 MFPlay 回放并等待结束事件；把结果写入日志，退出码 0 = PASS。

一条成功的日志示例（本机只有输出设备、没有麦克风，因此第二路为合成信号）：

```
=== SimpleRecorder self test ===
MFStartup: ok
系统声音采集初始化: ok - 48000 Hz / 2 ch / 32 bit
麦克风采集初始化: failed - 找不到默认麦克风（默认输入设备）设备 (0x80070490)
启动系统声音采集: ok
系统声音: 采集帧数 0, 存储格式 48000 Hz/2 ch, 文件 968176 字节, 峰值 0
本机无输入设备，生成合成第二路（44100 Hz / 单声道 / 正弦波）: ok, 文件 441044 字节
两路起始时间差: 2000000 (100ns)
系统声音 -> m4a: ok (5056 ms, 48000 Hz, 2 ch, 123423 字节)
合成第二路 -> m4a: ok (5015 ms, 44100 Hz, 1 ch, 122411 字节)
混合（两路对齐后合成） -> m4a: ok (5205 ms, 48000 Hz, 2 ch, 127314 字节)
回放 (MFPlay): ok
回放结束事件: 收到
结果: PASS
```

> 注意：`采集帧数 0` 说明当时系统完全静音，采集端点一个数据包都没给；程序按 QPC 补齐了整段静音，所以文件时长仍是正确的 5 秒 —— 这正是 2.2 节描述的机制。

### 界面回归测试

```powershell
powershell -ExecutionPolicy Bypass -File tests\verify-ui.ps1
```

它会启动程序并自动检查：窗口可按比例缩放、缩小被限制在最小尺寸、三个按钮尺寸与字体完全一致、
每个控件都完整落在客户区内、单选标签宽度足够、提示/状态文本高度不低于 worst-case 预留值、
按钮与模式的启用状态转换、录音结束后能否立即开始第二段录音、保存对话框能否正常打开与取消、
未保存退出时是否弹提示并在丢弃后清理临时文件。全部通过时输出 `RESULT: 0 failure(s)`（当前共 32 项）。

静音诊断的检查（录制一段无任何声音的系统声，确认会给出警告）：

```powershell
powershell -ExecutionPolicy Bypass -File tests\check-silence-warning.ps1
```

`--selftest` 现在还会：

1. **写入器格式自检**：对 float32 / int32 / int24 / int8 / int16 五种采集格式各写一份 WAV，
   校验文件字节数与峰值是否符合预期（真实设备几乎只会给出 float32，其余分支靠这个测试覆盖）；
2. 对每个输出文件做**解码后内容校验**：重新用 Media Foundation 解码生成的 m4a，统计峰值与帧数，
   只有当“源里有信号 ⇒ 输出里也有信号”且时长一致时才判 PASS（避免再出现“尺寸对但内容是静音”的回归）。

### 其他测试脚本

| 脚本 | 作用 |
| --- | --- |
| `tests\verify-ui.ps1` | 界面回归（32 项：缩放/最小尺寸/按钮一致/状态机/保存对话框/退出清理） || `tests\check-silence-warning.ps1` | 录制一段无声音的系统声，验证“全程没有声音”警告会出现 |
| `tests\check-mixed-gui.ps1` | 真实混合录音端到端（时长准确性、无警告、回放切换、退出清理） |

设置环境变量 `SIMPLERECORDER_TRACE=<文件路径>` 可以让程序输出布局量测（DPI、行高、标签文本宽度、
最小尺寸）与 `MfAudio` 各阶段跟踪日志，便于排查显示或编码问题。

---

### 自动构建与发布（GitHub Actions）

`.github/workflows/build.yml`：

| 触发 | 行为 |
| --- | --- |
| push 到 `main` / Pull Request / 手动触发 | 在 `windows-latest` 上用 MSVC 配置并编译（Release）→ 跑 `--selftest`（runner 无音频端点时按“跳过”处理）→ 把 exe 作为构建产物上传 |
| push 形如 `v*` 的 tag | 在以上基础上，把编译出的 `SimpleRecorder.exe` 上传（或替换）到该 tag 的 Release |

所以发布新版本的流程就是：

```powershell
cd <你的仓库工作副本>
# 1) 改代码 → 本地构建 + 跑 tests → git commit → git push origin main
# 2) 打 tag 并推送，CI 会自动编译并把附件挂到对应的 Release
git tag v1.0.1
git push origin v1.0.1
```

CI 日志里会打印构建产物的大小与 `sha256:`。注意：**Release 附件由 CI 构建**，与本地手工构建的哈希可能不同
（编译器/工具集版本差异），因此以 CI 日志中的校验值为准。

发布说明不是手写的：它来自 [`.github/release-notes.md`](https://github.com/dct74/SimpleRecorder/blob/main/.github/release-notes.md) 模板，
CI 在发布时把 `{{VERSION}}`、`{{COMMIT}}`、`{{BUILD_DATE}}`、`{{SIZE}}`、`{{SHA256}}` 填好后作为 release notes 使用；
若模板里残留未替换的 `{{...}}`，工作流会直接失败（防止写出带占位符的发布说明）。
所以今后的发布说明格式统一，且附带**与附件完全对应的 SHA-256**。

## 6. 故障排查

### 录了几秒，却显示“时长 5 分多钟”、合并也很久

**原因**：录音设备（驱动）返回了异常的时间戳，旧版程序把跳变的差值全部当成静音写进了文件，
于是 5 秒的录音变成 5 分钟的文件，合成时还要多编码这些静音。
**现状**：2.2 节的“时间轴不超前于真实时间”安全阀已经阻止这种情况 —— 文件长度只取决于真实录制时间，
并且状态区会提示“⚠ 录音设备返回了异常时间戳，已按真实录制时长忽略 X.X 秒静音”。

想进一步确认设备行为，可以开启跟踪日志后再录一段：

```cmd
set SIMPLERECORDER_TRACE=%TEMP%\sr-trace.txt
SimpleRecorder.exe
```

日志里 `capture start:` 行给出 QPC 频率与基准，`gap clamped:` 行给出被拒绝的静音帧数与真实上限。
也可直接跑 `--selftest` 把整条链路（采集 → 原生编码 → 混合 → 回放）的数值一次性打出来。

### 录音文件没有声音 / 只有一段静音

先看状态区有没有提示（新版会自动诊断，不需要猜）：

| 状态区提示 | 含义 | 怎么办 |
| --- | --- | --- |
| ⚠ 麦克风轨全程没有声音 | 麦克风一路全是数字零 | 检查麦克风是否被静音/音量归零、是否选错设备（系统默认输入）、硬件静音键 |
| ⚠ 系统声音轨全程没有声音 | 输出端点没送出任何数据 | 录制时确实没有任何程序在播放；或播放设备被独占/切到了另一个端点 |
| ⚠ 录音设备返回了异常时间戳… | 设备时间戳跳变 | 无需处理，已自动按真实时长裁剪；只影响了“哪里算静音” |

如果**状态区没有任何警告**却听不到声音，请用上面的自检模式确认整条链路：`--selftest` 会打印每份输出的**解码后峰值**，
峰值 > 200 就说明文件里确实有信号（还能看到“源峰值”供对比）。

> 旧版本曾出现“录了几秒却生成 5 分钟文件”的缺陷：真正的音频被排在了那几分钟静音的**后面**，
> 从头播放自然就是无声。该缺陷已由 2.2 节的“时间轴不超前于真实时间”修正。

### 长时间会话的资源占用

每录一段（含合成）进程内核句柄会增长约 7 个（实测：1 路与 2 路模式增长量相同，说明不是采集对象的泄漏，
而是 Media Foundation 自身每次初始化的开销），线程数稳定在 16 个、工作集稳定在 ~30 MB。
连录 8 段后约为 520 个句柄（上限量级为一万），因此正常使用不会触及限制；若需连续录上千段，建议中途重启一次程序。

### 录音结束后程序闪退（0xC0000005）

已修复：自检路径下 WASAPI/COM 接口的释放在 `CoUninitialize()` 之后发生，导致析构时访问已失效的接口指针。
现在 `AudioCaptureEngine::Close()` 会在 `CoUninitialize()` 之前显式释放所有 COM 对象。

## 7. 已知限制

1. **没有设备选择界面**：系统声音固定用默认输出设备，麦克风固定用默认输入设备（Windows 设置里改默认即可）。
2. **混合时两路各自跟随自己的设备时钟**：系统声与麦克风可能由不同硬件时钟驱动，长时间录制会有极微小漂移（约 0.01%，1 小时约 0.3 秒）；合成时以较长的一路为准，短的一路末尾补静音。
3. **相加采用单位增益**：两路同时接近满幅时会限幅（与本项目早期版本及上游 `AudioMixer` 的策略一致），不做自动压缩。
4. **加 32 位整数 PCM 格式的设备**：转换分支已修复并有自检覆盖（旧版本对这类设备会算错样本数、读到缓冲区外），
   但本机没有该格式的真机可以实测，建议遇到时先用 `--selftest` 确认（日志会打印存储格式）。
5. **回环采集不含"保护性静音"丢包之外的补偿**：若设备被独占、或采样率中途改变（`AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY`），只按时间戳补静音，不做重采样纠偏。
6. **需要 Media Foundation 的 AAC 编码器**：精简版 Windows（N/KN 版）需要安装 Media Feature Pack；若初始化失败会给出明确提示。
7. 提升权限的程序（以管理员运行的应用）其声音可能采集不到，除非本程序也以管理员运行。

## 8. 许可（License）

本项目采用 **MIT License**，全文见 [LICENSE](LICENSE)。

```
Copyright (c) 2026 dct74
```

本项目是针对同一类需求的独立实现：设计思路参考了上游 `AudioCapture` 项目公开的做法
（WASAPI 回环/输入采集、Media Foundation 的 AAC 与 MPEG-4 管线），本仓库全部代码为原创编写，
不含上游代码；如果你要二次发布，请遵守本仓库的 MIT 条款。
