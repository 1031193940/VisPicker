# P-06 真实内容质量输入

此目录固定两张 NASA 照片及一段 Blender 官方短片的真实连续帧，用于量化误差、静止重复帧稳定性和 GIF 视觉回归。全部图片合计 **1,429,882 字节**；原视频和下载压缩包留在忽略目录 `out/quality-source`，不进入仓库。文件尺寸、SHA256、下载 URL、来源和处理过程见 [SOURCES.json](SOURCES.json)。

## 素材来源与署名

- `nasa-iss23-atmosphere.jpg`：900×450，[NASA / ISS Expedition 23 日落照片](https://science.nasa.gov/image-article/apod-2010-june-23-sunset-from-the-international-space-station/)，2010-06-23 发布。保留官方提供的 JPEG，无额外缩放或重新压缩。
- `nasa-iss068e023088-sunset.jpg`：1280×853，[NASA 地球大气日落照片](https://www.nasa.gov/image-article/suns-last-rays-illuminate-earths-thin-atmosphere/)，2022-11-18 拍摄。保留官方 medium JPEG，无额外处理。两张图片用于技术质量测试，来源署名 NASA；适用 [NASA 媒体使用指引](https://www.nasa.gov/nasa-brand-center/images-and-media/)。
- `video-bbb/frame-0000.png` 至 `frame-0011.png`：从 [Blender 官方 Big Buck Bunny 下载](https://peach.blender.org/download/) 的 320×180 H.264 版本提取。署名：**(c) copyright 2008, Blender Foundation / www.bigbuckbunny.org**。依 [官方许可说明](https://peach.blender.org/about/) 使用 [CC BY 3.0](https://creativecommons.org/licenses/by/3.0/)；改动为取出视频片段并将解码 RGB 写为无损 PNG，无裁剪、缩放、补帧、跳帧或合成运动。

视频输入是 **24fps 的连续 12 帧**，实际时间戳为 30.0000000–30.4583333 秒；间隔约 41.6667ms，记录于 [video-bbb/frames.csv](video-bbb/frames.csv)。这不是 10fps 抽帧。基准输出的 `quality_ordered_inputs_0.gif` 固定按 **10fps 慢放**，仅方便观察量化抖动，不能用其播放时长推断原视频节奏。

## 运行质量基准

在仓库根目录运行已构建的开发入口。输入扫描不递归，按文件名排序，只读取 PNG/JPG/JPEG/BMP，最多 64 个文件。根目录默认只测试两张照片；视频另跑一次。`VISPICK_QUALITY_INPUT` 也可指向自己的公开或有权使用的图片目录。

```powershell
Remove-Item Env:VISPICK_QUALITY_INPUT -ErrorAction SilentlyContinue
.\out\build\x64-Release\VisPickTest.exe --spike-quantizer --output .\out\reports\photo-quality

$env:VISPICK_QUALITY_INPUT = (Resolve-Path .\docs\reports\data\quality-input\video-bbb).Path
.\out\build\x64-Release\VisPickTest.exe --spike-quantizer --output .\out\reports\video-quality
Remove-Item Env:VISPICK_QUALITY_INPUT
```

报告中 `real_N input=...` 将编号映射到文件。每张图记录不复用调色板的三次量化中位数/吞吐、Wu 路径标识，fresh 与 temporal 的 RGB 各通道最大误差/RMSE、RGB RMSE/PSNR；零误差时 PSNR 为无穷，因此省略该项。静止重复帧要求 palette 和 index 差异为零。每张量化 GIF 通过 WIC 独立解码，与量化后的 RGB 对照；误差指标比较原始解码图像与量化图像，不把有损量化误报为逐像素无损。

`real_N_quantized.gif` 保存各图，`quality_ordered_inputs_N.gif` 按连续相同尺寸的组保存动画。计时仅包括量化，排除 WIC 输入解码、缩放、误差统计和 GIF 写入。输入超出 1920×1080 才等比例 Fant 缩小。误差在解码后的 8 位 RGB 码值空间计算，不应用 ICC、EXIF 方向或线性光转换；目前不加抖动。

## 复现视频提帧

从 `SOURCES.json` 指定的 Blender 官方 URL 下载压缩包，核对 SHA256，再将唯一指定成员 `BigBuckBunny_320x180.mp4` 解到 `out/quality-source` 并核对成员哈希。附带的 [ExtractVideoFrames.cpp](ExtractVideoFrames.cpp) 是独立素材准备工具，使用系统 Media Foundation 软件解码和 WIC PNG 编码，不属于产品或 CMake 目标，也不引入第三方运行时依赖。

```powershell
. .\scripts\Enter-VsEnvironment.ps1 -Architecture x64
cl /nologo /std:c++20 /O2 /EHsc /MT /W4 /WX /utf-8 /DUNICODE /D_UNICODE /DNOMINMAX /Fe:out\quality-source\ExtractVideoFrames.exe /Fo:out\quality-source\ExtractVideoFrames.obj docs\reports\data\quality-input\ExtractVideoFrames.cpp /link mfplat.lib mfreadwrite.lib mfuuid.lib windowscodecs.lib ole32.lib
.\out\quality-source\ExtractVideoFrames.exe .\out\quality-source\BigBuckBunny_320x180.mp4 .\out\quality-source\reproduced-frames 30 12 > .\out\quality-source\reproduced-frames.csv
```

参数依次为源视频、输出目录、开始秒数、连续帧数。工具输出的是解码器给出的实际时间戳，不是推算帧号。素材生成环境是 Windows 11 build 26200；不同系统解码器/PNG 编码器版本可能产生不同 RGB 舍入或 PNG 字节。仓库中的 PNG SHA256 固定了本次质量基准的实际输入。

这些素材只覆盖两张大气渐变照片和约半秒的低分辨率渲染影片片段。数值检查通过不代表主观色带/闪烁通过，更不代表高分辨率真实视频性能验收；仍需要人工检查输出 GIF，并在有效本地显示环境和更广泛内容上验收。
