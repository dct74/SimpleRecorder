**单文件原生 Windows 录音工具：系统声音 / 麦克风 / 混合录制 → M4A**，免安装、无第三方依赖。

### 下载

| 文件 | 说明 |
| --- | --- |
| `SimpleRecorder.exe` | {{SIZE}}，x64。拷到任意 Windows 10/11 x64 机器双击即可用，无需安装、无需任何运行时 |

```
SHA-256  {{SHA256}}
```

> 本附件由 GitHub Actions 在 `windows-latest` 上用 MSVC 构建（提交 `{{COMMIT}}`，构建日期 {{BUILD_DATE}}）。
> 校验方式：`Get-FileHash .\SimpleRecorder.exe -Algorithm SHA256`

### 三种录制模式

| 模式 | 说明 |
| --- | --- |
| ① 只录制系统声音 | 默认输出设备的 WASAPI 回环采集（电脑正在播放的声音） |
| ② 只录制麦克风声音 | 默认输入设备，共享模式，跟随设备混音格式 |
| ③ 混合录制 | 系统声音与麦克风**分两路独立采集**，停止后用 **Windows 原生 Media Foundation 管线**对齐、相加、编码为一个 `.m4a` |

界面只有：三个模式单选、`录制/停止`（单按钮切换）、`回放`、`保存为 M4A`；窗口可自由缩放、随显示器 DPI 缩放，状态区给出每路诊断。

### 本版要点

* 单文件、静态 CRT：导入表只含 11 个系统 DLL，不需要 VC++ Redistributable，不需要 vcpkg
* 两路**帧级时间轴对齐**：设备包时间戳用于把静音正确补进时间轴，但录制时间轴**永远不会超前于真实经过时间**（+2 秒余量）——异常驱动的跳变时间戳不会再让 3 秒录音变成几分钟
* **明确报错而不是静默失败**：写盘失败 / 4 GB 上限、设备中途掉线、某一路全程无信号、被拒绝的异常时间戳，都会在状态区写清楚
* `--selftest` 可在无界面下自检整条链路（写入器 5 种采样格式转换 → 采集 → 原生重采样 → 混合 → AAC 编码 → 回放），并**重新解码**产出的 m4a 校验信号真的存在（退出码：0 = 全通过，2 = 本机无音频端点故跳过采集部分，1 = 失败）
* 输出：AAC-LC 192 kbps / MPEG-4 容器（`.m4a`）

### 系统要求

Windows 10 / 11 x64（Windows 7 SP1+ 理论上可用，未实测）。依赖系统自带的 **Media Foundation** 与 **WASAPI**：
Windows N/KN 版需安装微软免费的 *Media Feature Pack*；S 模式无法运行非商店应用；ARM64 版 Windows 11 靠 x64 仿真运行。

### 已知限制

两路时钟极微小漂移（1 小时约 0.3 秒，以较长一路为准）、相加采用单位增益（两路同时满幅会限幅）、
单文件 4 GB 上限（约 5.6 小时 48 kHz 立体声）、合成期间不可取消、没有设备选择与格式选项。
详见 [README 的"已知限制"](https://github.com/dct74/SimpleRecorder#7-已知限制)。

---

使用说明、设计原理与构建方式见 [README](https://github.com/dct74/SimpleRecorder#readme) · License: [MIT](https://github.com/dct74/SimpleRecorder/blob/main/LICENSE)
