# VisPick 0.5.3 Windows 11 便携预览版

VisPick 是一款常驻托盘的截图、贴图和 GIF 录制工具。本次预览面向 **Windows 11 x64**，版本为 `0.5.3-candidate`。这一版减少了 PNG 编码时的整图复制和 GIF 编码线程的内存分配。

## 开始使用

1. 如果旧版仍在运行，先从托盘菜单退出旧版。
2. 把 `VisPick-0.5.3-candidate.zip` 解压到可写目录，双击 `VisPick.exe`。程序启动后常驻系统托盘。包内的 `VisPick-x86.exe` 是 32 位构建，也在 Windows 11 x64 的兼容层下做过回归。
3. 首次启动会生成带中文注释的 `VisPick.ini`。程序目录不可写时，配置会写入 `%APPDATA%\VisPick\VisPick.ini`。编辑配置并保存即可生效。热键冲突时，可在 `[Hotkeys]` 中更换对应按键。

| 操作 | 默认热键与步骤 |
|---|---|
| 截图和标注 | `Ctrl+1`，拖出选区或单击窗口。可添加矩形、箭头、文字和马赛克。按 `Enter` 或双击复制，按 `Ctrl+S` 保存为 PNG、JPG 或 BMP。 |
| 贴图 | `Ctrl+3`，框选后按 `Enter` 把参考图钉在屏幕上。滚轮缩放，`Ctrl` 加滚轮调整透明度。 |
| 录制 GIF | `Ctrl+2`，框选后按 `Enter` 开始。再次按 `Ctrl+2` 或点击停止进入预览，在预览中按 `Ctrl+S` 保存。单次录制最长 60 秒。 |

包内两份 exe 均未签名，从网上下载后可能出现 Windows SmartScreen 提示。 [Microsoft SmartScreen 说明](https://learn.microsoft.com/en-us/windows/apps/package-and-deploy/smartscreen-reputation)

## 已验证的范围

- 在 Windows 11 x64 的单屏 RDP 会话（2560×1440、125% 缩放、32 Hz）中，x64 和 x86 构建均通过输出、选区、编辑、贴图、录制、应用生命周期和常驻回归。四种 Debug 和 Release 构建配置各通过 293 项单测。
- 两架构的 60 秒图样录制完成后，解码帧的像素均与预期图样一致。同一份短 GIF 在 Chrome、Edge、Firefox 和 Windows 照片中通过播放抽样。
- 约 12 分钟的循环测试中，私有提交峰值相对暖机基线增长 2.18%，GDI、USER 和句柄增长均低于 5%。包内两份 exe 的哈希与 Defender 定向扫描的文件一致，扫描未发现威胁。

## 尚未验收的项目

- 干净系统上的首次运行，以及 Windows 10、Windows 7、Windows 8.1 和 Windows 11 ARM64 的运行兼容性。
- Windows 11 本地物理桌面的热键响应、30 fps 录制性能、混合 DPI 和双 4K。x64 的 1080p、60 秒录制在上述 RDP 环境中丢失 6.22% 的采集节拍，物理桌面表现待测。
- 真实中文输入法的组合输入、第三方应用粘贴，以及微信和 QQ 的 GIF 播放。
- 八小时稳定性验收（AC6-3）本轮跳过。

## 文件校验

`VisPick-0.5.3-candidate.zip`（742,311 字节）的 SHA-256

```text
B1D6DDBF1E80D856D99979623EE4D77E78F4C1A5BC2838A0178D1B50B63CEC7C
```
