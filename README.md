# VisPick

VisPick 是一款免费的 Windows 截图、参考图与 GIF 工具。快速截取画面、添加标注，把参考图贴在桌面上，或录下操作过程分享给他人。

[下载 VisPick_0.7.0.exe](https://github.com/1031193940/VisPicker/releases/download/v0.7.0/VisPick_0.7.0.exe) · Windows 11 x64 · [MIT 开源](LICENSE)

## 产品演示

点击封面观看 30 秒演示，了解截图标注、桌面贴图和 GIF 录制。演示画面来自 0.5.9。

[![VisPick 产品演示](docs/media/vispick-poster.webp)](docs/media/vispick-promo.mp4)

| 截图与标注 | 桌面参考图 | GIF 录制与预览 |
|---|---|---|
| [![截图与标注](docs/media/vispick-screenshot.webp)](docs/media/vispick-screenshot.webp) | [![桌面参考图](docs/media/vispick-pin.webp)](docs/media/vispick-pin.webp) | [![GIF 录制与预览](docs/media/vispick-gif.webp)](docs/media/vispick-gif.webp) |

## 使用操作

下载后双击运行，VisPick 会驻留在系统托盘。右键托盘图标可打开菜单、管理参考图或退出程序。

### 截图与标注

按 **Ctrl+1**，拖动框选区域或单击窗口。选好后可以调整范围，添加矩形、椭圆、箭头、画笔、文字或马赛克。

点击已有标注可选中并拖动。矩形和椭圆可拖动手柄调整大小，箭头可拖动端点，双击文字可再次编辑。工具栏下方显示当前标注可调的属性。

- **对勾**：复制到剪贴板。
- **软盘**：保存为 PNG、JPG 或 BMP。
- **图钉**：将截图变为桌面参考图。

将鼠标停在图标上，可查看操作名称和快捷键。

### 桌面参考图

拖动参考图移动位置，滚轮缩放，Ctrl+滚轮调整透明度。双击可收缩或恢复，右键可设置置顶和鼠标穿透。

参考图默认留在当前虚拟桌面。需要在不同桌面间共享时，打开托盘菜单中的「参考图」→「参考图跨桌面」。

### GIF 录制

按 **Ctrl+2**，框选区域并选择帧率，点击录制按钮。录制中可以暂停、继续；点击停止按钮或再按 Ctrl+2 打开预览。

拖动进度条查看画面，用起点和终点按钮保留需要的片段。点击双页图标复制 GIF，或点击软盘图标保存。

## 快捷键

**Ctrl+1 / Ctrl+2 为全局快捷键**，其余快捷键在对应窗口中使用。

### 截图与标注

| 快捷键 | 操作 |
|---|---|
| Ctrl+1 | 开始截图 |
| Ctrl+C | 复制截图 |
| Enter | 编辑选中的文字，其他情况下复制截图 |
| Ctrl+S / Ctrl+T | 保存截图 / 创建参考图 |
| R / E / A / P / T / M | 矩形 / 椭圆 / 箭头 / 画笔 / 文字 / 马赛克 |
| Ctrl+Z / Ctrl+Y | 撤销 / 重做 |
| Delete | 删除选中的标注 |
| 方向键 | 移动选中的标注或截图选区，按住 Shift 加快移动 |
| Ctrl+方向键 | 未选中标注时调整选区尺寸 |
| Ctrl+A / Tab / Shift+Tab | 选择当前显示器 / 下一个窗口 / 上一个窗口 |
| C / Shift+C | 复制 HEX / RGB 颜色值 |
| Esc | 结束文字输入、取消标注拖动或选择，或退出截图 |

### GIF

| 快捷键 | 操作 |
|---|---|
| Ctrl+2 | 开始 GIF 选区；录制中停止并打开预览 |
| 空格 | 预览播放 / 暂停 |
| `[` / `]` / Ctrl+R | 设置片段起点 / 设置终点 / 恢复完整录制 |
| Ctrl+C / Ctrl+S | 复制 / 保存当前片段 |

### 参考图

| 快捷键 | 操作 |
|---|---|
| 方向键 / 滚轮 / Ctrl+滚轮 | 移动 / 缩放 / 调整透明度 |
| R / Alt+H / Alt+V | 旋转 / 水平翻转 / 垂直翻转 |
| Ctrl+C / Ctrl+X / Ctrl+V / Ctrl+S | 复制 / 剪切 / 从剪贴板新建参考图 / 保存 |
| Esc 或 Q / Ctrl+Z | 关闭参考图 / 恢复最近关闭的参考图 |
