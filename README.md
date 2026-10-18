# 专业拧瓶盖员

轻量 C++17 无线 CarPlay 接收端。协议、会话和 Qt 界面在一个进程里，不依赖 Electron、Chromium 或 Node。

界面名称和图标是「专业拧瓶盖员」。默认设备名也是这个名字。

## 能做什么

- BlueZ RFCOMM 上的 iAP2
- `resources/authentication/` 里的 `identity.pk8` 与 `certificate.p7b` 做 MFi 签名
- pair-setup / pair-verify
- TCP 17000 上的控制通道、H.264 画面、音频和触控
- Qt5 设置窗口和全屏画面
- `scripts/carplay-hotspot.sh`：用 hostapd 开带 Apple Device IE 的 2.4 GHz 热点

本机设置、配对记录和画面转储只写在 `~/.config/carplay-cpp/`，不要放进仓库。

## 编译

```sh
make test
make ui    # 需要 qtbase5-dev、libbluetooth-dev、libssl-dev、libavcodec-dev、libswscale-dev
./build/carplay-ui
```

也可以用 CMake：

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCP_UI_QT5=ON
cmake --build build
```

Linux 上还需要 BlueZ。热点脚本另外需要 root 权限的 `hostapd` 和 `dnsmasq`。Intel AX201 一类网卡只能开 2.4 GHz 热点。

## 配置

复制 `settings.example.json` 的字段含义即可。程序第一次启动会在 `~/.config/carplay-cpp/settings.json` 里写入自己的配置。Wi-Fi 名称、密码和蓝牙地址留空，在界面里填。

认证文件已经放在 `resources/authentication/`。若该目录缺少文件，可在界面里导入自己的 `identity.pk8` 与 `certificate.p7b`。说明见 `resources/authentication/SOURCE.txt`。

## 踩过的坑

这些是把画面从「能出」调到「能看清」时实际碰到的限制。码率数字来自这台 Intel AX201 笔记本上的测量，别的网卡会不一样。

### 画面糊，先看手机编码码率

糊的主因是 iPhone 把 H.264 压在大约 1 Mbps，不是本机缩放。TCP 接收窗口已经开到数 MB，手机仍然只送约 1 Mbps：1920×1080、大约 25 fps，关键帧常常只有几 KB 到十几 KB。把分辨率改成 2560×1440 再缩回 1080p 面板，只会更糊。像素要和屏幕 1:1，这台是 1920×1080。

`/info` 里的 `maxBitRate` 基本不起作用。不写这个字段，和 MacPlay 一样。手机看的是当前 Wi-Fi 的 `AverageTxCapacity`；读不到可用带宽时，编码下限大约就是 1 Mbps。

### 局域网提不了码率

手机连的是家用路由器时，信标里没有 Apple Device IE（OUI `00:A0:40`，Flags 里要有 CarPlay 位）。iOS 不把这条网络当成 CarPlay 热点，容量按缺失处理，码率停在大约 1 Mbps。电脑侧改 `/info`、加大 socket 缓冲、关掉逐帧写盘，都改变不了手机的发送量。

本机热点脚本会在 2.4 GHz（信道 1、6 或 11）的 hostapd 信标里带上这个 IE。手机改连这个热点后，码率可以从大约 1 Mbps 抬到大约 10 Mbps。

### 能连 5 GHz，不能开 5 GHz 热点

AX201 做客户端可以连 5 GHz 路由器，做热点时驱动把全部 5 GHz 信道标成 `no IR`（不允许自己发信标）。hostapd 在第 40 信道上报 `NO-IR`。已装固件 `Qu-c0-hr-b0-77` 是目前公开发布的最新一版，降到 66/72/73/74 也一样。当前内核里的 `iwlwifi` 没有旧的 `lar_disable` 参数。

因此「现有局域网」下不要强行在 5 GHz 同一信道再加一个热点，否则启动接收会直接失败。要更高码率需要第二块允许 5 GHz AP 的网卡（例如 MT7921AU），信道用 149、153、157 或 161。Apple 建议 CarPlay 走 5 GHz；2.4 GHz 只允许 HT20，再和蓝牙分空口，大约 10 Mbps 是这块 Intel 网卡上的正常水位。

### 图标锯齿是物理尺寸，不是抗锯齿开关

把真实屏幕毫米（例如 340×190 mm，约 143 PPI）报给手机，图标会按大像素画，边缘台阶很明显。分辨率不变，把物理尺寸改成大约 240 PPI（1920×1080 大约是 203×114 mm），手机会用更密的资源，边缘平滑一些。这解决不了 1 Mbps 编码造成的整幅发糊。

### 解码

手机送的是 full-range 的 `yuvj420p`。转 RGB 时要用 JPEG 全范围，否则颜色发灰。FFmpeg 会打 `deprecated pixel format`，可以忽略。

### 界面和启动

全屏会挡住日志。码率、帧率、关键帧要单独显示，不要只写在日志里。

Qt 在 Wayland 会话里对 `requestActivate()` 会报错。启动前设置 `QT_QPA_PLATFORM=xcb`。

不要把 `~/.local/bin/carplay-ui` 做成指向 ELF 的符号链接再往这个路径里写启动脚本，写入会盖掉二进制，进程会空转、没有窗口。启动脚本必须是普通文件，再 `exec` 真正的 `build/carplay-ui`。

热点脚本用 `sudo` 拉起。若没有免密 sudoers，`popen` 会卡在密码提示上。只给 `/usr/local/sbin/carplay-hotspot` 配 `NOPASSWD`，脚本本体不要放在用户可写目录里。

`~/.config/carplay-cpp/settings.json` 里有 Wi-Fi 密码。不要提交，也不要打进日志。

## 许可

代码使用 GNU GPL-3.0-or-later，见 `LICENSE`。
认证材料的来源和限制见 `resources/authentication/SOURCE.txt`，与代码许可证分开。
