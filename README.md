# FFmpegFreeUI Native

原生 **C++23 / Qt 6** 改写版，支持 Windows 和 Linux，Linux 优先使用原生 Wayland，同时支持 X11/XCB。

原出处：[Lake1059/FFmpegFreeUI](https://github.com/Lake1059/FFmpegFreeUI)。原作者 **Lake1059 / 1059 Studio**。基于上游 **6.2.36**，提交 [`65aec1ff4dcd62a54c4361fe1719520378750c36`](https://github.com/Lake1059/FFmpegFreeUI/tree/65aec1ff4dcd62a54c4361fe1719520378750c36)。此仓库是独立改写版，不代表原作者维护的官方版本。

C++ 改写分支：[zybin7890/FFmpegFreeUI · native-cpp-linux](https://github.com/zybin7890/FFmpegFreeUI/tree/native-cpp-linux)。原版 Windows / VB.NET 实现在上游及本 fork 的 `main` 分支保留，本分支改为独立的原生 CMake 工程。

新写的 C++ 代码及构建配置采用 **AGPL-3.0-only**，完整协议见 [LICENSE.txt](LICENSE.txt)。原版预设字段、11 个内置预设、图标保留原 MIT 版权和许可，见 [LICENSES/Upstream-MIT.txt](LICENSES/Upstream-MIT.txt)。第三方组件见 [THIRD-PARTY-NOTICES.txt](THIRD-PARTY-NOTICES.txt)。分发二进制时须同时提供本版本的完整对应源码与构建文件；随附的源码压缩包可用于重新构建。

## 已实现的功能

- 中文原生界面、拖入文件、批量准备、队列重排、并行数设置。
- FFmpeg 子进程启动、实时进度、速度、输出大小、剩余时间、日志、失败显示。
- Linux 使用 SIGSTOP/SIGCONT 暂停与恢复，Windows 使用任务进程的线程暂停与恢复。停止操作仅针对本程序拥有的子进程。
- 每个任务独立二次编码日志；未完成队列原子保存、重启恢复为等待状态。
- 输出命名、随机数字/字母后缀、补零序号、保留子目录和原文件修改/访问时间；创建时间保留仅限 Windows。
- v6 `.3fui` JSON 读写、186 个字段编辑、11 个原版内置预设、未知字段原样保留。
- 软件 / 硬件编码器、CRF/CQP/码率控制、二次编码、裁剪、缩放、抽帧、插帧、混帧、libplacebo 超分、降噪、锐化、颗粒、去色带、扫描、翻转、烧字幕、色彩、音频滤镜和自定义滤镜排序。
- 常规剪辑、流选择、元数据、章节、字体/文本附件、自定义参数和完整自写模式。
- ffprobe 信息、ffplay 播放、简单混流、concat 合并、SSIM/PSNR 质量评测。
- 无界面 CLI 转码、探测与命令预览；调用参数列表直接传给程序，不经过 shell。
- DEB、RPM 的 CPack 配置、桌面入口和系统图标。

**本版本尚未完成原版全部功能的 1:1 迁移。** 原版 .NET 插件、Agent 智能体、LibreHardwareMonitor、网络更新器、付费个性化和远程接口未移植。NV_FRUC、VapourSynth 管线、剔除中间区间、选择部分流并复制其他流、旁挂字幕自动混流、封面图流映射会明确报错；可手动编写相应 FFmpeg 参数。Linux 不提供原版 AviSynth Windows 后端。参数可编辑和预设可保存不等于相关后端已经可用。硬件/滤镜实际能力取决于安装的 FFmpeg 和驱动。

为保护源媒体和已有结果，本版本使用 `-n`，输出不覆盖源文件或已有文件；自定义命令里的 `-y` 会被移除。停止/失败产生的部分文件保留，重试请先选择新的输出路径。每个加入队列的任务保存独立预设快照，之后修改面板只影响新任务。

## 构建 Linux

Debian 12 / Ubuntu 24.04 及更新版本：

```sh
sudo apt install g++ cmake ninja-build qt6-base-dev qt6-wayland ffmpeg rpm dpkg-dev
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build -j2
ctest --test-dir build --output-on-failure
cpack --config build/CPackConfig.cmake -G DEB
cpack --config build/CPackConfig.cmake -G RPM
```

Fedora（RPM 应优先在目标发行版构建，以匹配系统 ABI）：

```sh
sudo dnf install gcc-c++ cmake ninja-build qt6-qtbase-devel qt6-qtwayland ffmpeg-free rpm-build
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build -j2
cpack --config build/CPackConfig.cmake -G RPM
```

发行版的 `ffmpeg-free` 可能不提供 libx264 等编码器，需选择该 FFmpeg 实际支持的编码器，或由用户安装包含所需编码器的 FFmpeg。当前核心集成测试需要 libx264 和 AAC。

```sh
sudo apt install ./ffmpegfreeui-native_0.1.0-1_amd64.deb
# Fedora 中安装在 Fedora 构建的包：
sudo dnf install ./ffmpegfreeui-native-0.1.0-1.x86_64.rpm
```

## Wayland / X11

启动时检测会话：存在 `WAYLAND_DISPLAY` 或 `XDG_SESSION_TYPE=wayland` 时优先原生 Wayland；X11 会话选择 XCB。不依赖 XWayland 运行原生 Wayland 界面，保留系统窗口装饰、DPI、剪贴板、输入法和文件对话框机制。显式 `QT_QPA_PLATFORM` 或 Qt `-platform` 选项优先，不改用户的全局环境。

```sh
ffmpegfreeui
FUI_DISPLAY_BACKEND=wayland ffmpegfreeui
FUI_DISPLAY_BACKEND=x11 ffmpegfreeui
QT_QPA_PLATFORM=wayland ffmpegfreeui
ffmpegfreeui -platform xcb
```

DEB 依赖 `qt6-wayland`，Qt/XCB 运行库由自动共享库依赖解析补充；RPM 依赖 `qt6-qtbase-gui`、`qt6-qtwayland` 和 FFmpeg 可执行文件。普通运行不需要 root。用户数据使用 Qt 的 XDG 数据/配置目录，临时文件使用系统临时目录。

## Windows

安装 Qt 6.4+ 的 MSVC 或 MinGW 套件，必须匹配 Qt 的编译器与运行库（例如 Qt 6.8.3 MinGW 套件配 MinGW 13.1）。

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH='D:/Qt/6.8.3/mingw_64'
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
windeployqt --release --no-translations build/ffmpegfreeui.exe
```

FFmpeg / ffprobe / ffplay 从 PATH、程序目录或软件设置中寻找。Linux 包使用发行版的动态 Qt 库；Windows 便携版须随附动态 Qt 库、Qt 许可文本和对应源码获取信息。

## CLI

```sh
ffmpegfreeui --probe --input source.mkv
ffmpegfreeui --print-command --input source.mkv --output result.mp4 --preset profile.3fui
ffmpegfreeui --transcode --input source.mkv --output result.mp4 --preset profile.3fui
```

源码内的 `--render` 只用于渲染验收，截图后退出，并不读写普通用户预设/队列。

验证记录见 [VALIDATION.md](VALIDATION.md)。
