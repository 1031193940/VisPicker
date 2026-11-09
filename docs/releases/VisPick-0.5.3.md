# VisPick 0.5.3 Windows 11 便携版

VisPick 是一款免费使用的截图、贴图和 GIF 录制工具。本版面向 Windows 11 x64。包内的 `VisPick.exe` 是 x64 构建，`VisPick-x86.exe` 是 32 位构建。

## 开始使用

1. 从托盘菜单退出正在运行的旧版。
2. 将 `VisPick-0.5.3.zip` 解压到可写目录，双击 `VisPick.exe`。启动后程序常驻系统托盘。
3. 首次启动会生成带中文注释的 `VisPick.ini`。程序目录不可写时，配置保存在 `%APPDATA%\VisPick\VisPick.ini`。编辑后保存即可生效，热键冲突可在 `[Hotkeys]` 中改键。

| 操作 | 默认热键与步骤 |
|---|---|
| 截图和标注 | `Ctrl+1`，拖出选区或单击窗口。可在选区上标注。按 `Enter` 或双击复制，按 `Ctrl+S` 保存为 PNG、JPG 或 BMP。 |
| 贴图 | `Ctrl+3`，框选后按 `Enter` 把参考图钉在屏幕上。用滚轮缩放，按住 `Ctrl` 滚动可调整透明度。 |
| 录制 GIF | `Ctrl+2`，框选后按 `Enter` 开始。再次按 `Ctrl+2` 或点击停止进入预览，按 `Ctrl+S` 保存。单次录制最长 60 秒。 |

两份可执行文件均未签名，从网上下载后可能出现 Windows SmartScreen 提示。 [Microsoft SmartScreen 说明](https://learn.microsoft.com/en-us/windows/apps/package-and-deploy/smartscreen-reputation)

## 验证范围

- x64 和 x86 Release 构建各通过 293 项单测。两份可执行文件的 `--version` 均为 `VisPick 0.5.3`，Defender 定向扫描均未发现威胁。
- 本机短回归通过 x64 输出、选区、编辑、贴图和 GIF 录制，以及 x86 输出。x64 生命周期和常驻自动化、x86 选区自动化仍有待复测的用例。
- 此前预览版的同一份短 GIF 在 Chrome、Edge、Firefox 和 Windows 照片中通过播放抽样。

## 待验证项目

- 干净系统上的首次运行，以及 Windows 10、Windows 7、Windows 8.1 和 Windows 11 ARM64 的兼容性。
- 双 4K、物理混合 DPI，以及不同硬件上的热键响应和录制性能。两次本地 60 秒录制试跑中均有其他窗口进入采集区域，画面一致性待复测。
- 真实中文输入法的组合输入、第三方应用粘贴，以及微信和 QQ 的 GIF 播放。

八小时浸泡保留为可选诊断。 [P6 复测记录](https://github.com/1031193940/VisPicker/blob/v0.5.3/docs/reports/P6-memory-2026-11-06.md)
