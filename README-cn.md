# ESP32-S31 Linux eXtends

**X = X11 + eXtends**。这是
[annoyedmilk/esp32-s31-linux](https://github.com/annoyedmilk/esp32-s31-linux)
的 fork:俺们把**硬件加速的 X11 桌面**搬上了 ESP32-S31-Korvo-1。
中不中?你看——

**X framebuffer 原生导出**(`/dev/fb0` 直接 dump,3x 最近邻放大,
一个像素都没糊弄):

![IceWM 桌面,framebuffer 原生导出](docs/desktop-x11.png)

**实拍**(盯板的摄像头拍的,摩尔纹都是白送的):

![实拍](docs/desktop-photo.jpg)

English: [README.md](README.md)。

## 俺们弄啥嘞

上游(S31 上跑 Linux 的祖师爷)在 v0.7 里有这么句话:

> "An X server and a window manager do not fit in the approximately 3 MB
> that are free with Wi-Fi up, so the desktop is the console."
> —— 翻译:X 和窗口管理器塞不下,桌面就是个控制台。

俺寻思:320 MHz、16 MiB PSRAM、自带 2D 图形加速器,凭啥塞不下?
于是就有了这个 repo。**下边这些,全是上游没有的。**

### 1. X11,还是带硬件加速的那种

上游 fbdev 路径下,每个像素都得过一遍 320 MHz 软浮点 CPU,全屏填充
15.7 fps。这个 fork 写了个 EXA 驱动 `xf86-video-s31`,把活儿丢给硬件:

| EXA 操作 | 引擎 | 实测 |
| --- | --- | --- |
| Solid(矩形填充) | PPA fill | ~213 MB/s,全屏填充 **89 fps** |
| Copy(屏到屏) | DMA2D M2M | ~106 MB/s,滚动 **256 fps** |
| Composite | 故意**不挂** | 见下 |

Composite 为啥不挂?PPA blend 只有固定 per-plane alpha,而没有合成器的
桌面发出的 Render 全是"纯色 × a8 逐像素 mask"的抗锯齿字形——固定
alpha 硬件表达不了这个。硬挂上只会买到诊断,买不到帧。完整分析写在
`s31_exa.c` 注释里,可去盘道。

桌面配置:IceWM + 壁纸 + `xclock` + GT1151 触摸(i2c-gpio 轮询)+
USB 键鼠,开机 `startde` 一把梭。

### 2. F-only FPU 内核补丁

上游 Limits 一节写着"硬件 FPU 不用"——这颗芯有 F 没 D,而 Linux 内核
历来要求俩都得有。俺们的
`linux/patches/0015-riscv-f-only-fpu.patch` 教会内核认 `rv32imafc`:
FPU 上下文切换、hwcap,都齐了。用户态照旧 soft-float `ilp32`,老二进制
一个不动;想显式用 F 指令的程序(浮点 DSP 之类)现在能用了。
`/usr/bin/fputest` 打印 `FPUTEST_PASS`,不虚。

### 3. 冷静桌面:把 14 MB 工作集塞进 10.6 MiB 的手艺

Linux 只分到 16 MiB PSRAM 里的 10.6 MiB(内核、OpenSBI、framebuffer
把剩下的吃走了),而 X 桌面的工作集约 14 MB。在无头服务器上调这套
东西,每一种死法都看得清清楚楚,以下是学费换来的:

- **Wi-Fi auth 风暴,还藏着三个窝。** ESP-Hosted 对连不上的 AP 重试
  auth,能把整个 10 MiB 用户态活活饿死(约 8 分钟,连 X 一起冻)。
  这个 fork **不带任何 `wpa_supplicant.conf`**——Buildroot 包默认的
  那份(一个开放网络块,见 AP 就扑)、p1 上带真凭据的模板,全都断了。
  Wi-Fi 改 opt-in:自己的 conf 丢 p1 上,`S40wifi` 优先用它。
  `NOX` 那个"落回纯控制台"的老开关也还在。
- **kswapd 回收活锁。** zram 6M 被匿名页塞满之后,kswapd 开始抢 page
  cache,代码页从 SD 卡反复回读:40% iowait、idle 归零、load 上
  5.85、串口装死——而每个进程都还活着,可中个锤子。zram 提到
  12 MiB(lz4 压缩约 3 倍,pool 按需增长)+ `xterm` 只在检测到 USB
  键盘时才启动,load 5.85 变回 1.0,桌面还魂。得劲!
- **RGB 状态灯默认关。** 它漏光进盯板的摄像头(眩光 + 偏色,谁盯
  谁知道)。p1 上 `touch RGB` 再开。
- **`S70diag`:串口死了,卡自己会说话。** 每次开机自动采 15 分钟
  load/cpu/mem/swap,外加一个 8 MB SD 读速测试,全写进 p1 的
  `diag.txt`——拔卡就能看,不用伺候串口脾气。

### 4. 小件儿

- `icewm` 打 `SKIP_TRANSLATIONS=ON`:上游 po 机制用绝对路径装
  `.mo`,宿主 cmake ≥ 3.28 会给它垫上 DESTDIR 前缀,把文件装出
  target 边界,被 buildroot 扫描器当场拿下。
- `jpegdec`/`jpegenc` 硬编解码,桌面礼仪版(保存/恢复 PPA staging
  寄存器,不跟 X 抢屏)。
- `fputest-abi`、`s31view`、`scripts/cam-shot.sh`(固定对焦拍板,
  时间戳命名)、`scripts/flash-sd-linux.sh`(上游烧卡脚本是 macOS
  专用,这里补了 Linux 版,SHA-256 回读校验)。
- **纯原生构建**:x86 Linux 服务器上不套 Docker 直编
  (`scripts/soft-icewm-native-build.sh`,96 核冷缓存约 25 分钟)。

## 与上游的关系

本 fork 分叉于上游 `v0.6.0` 之前。上游后来单独演进:加了第二块板
(Function-CoreBoard-1,带以太网)、原生 I2C master 驱动(GT1151
触摸也换成了硬件 I2C)、tmux 控制台桌面,还有一整套 release 流程——
见 [docs/README-upstream.md](docs/README-upstream.md)。这些还没合并
回来,在路线图上。反过来,上游没有 X11,也没有 FPU 补丁。想要上游
全家桶,直接用
[上游 release](https://github.com/annoyedmilk/esp32-s31-linux/releases),
中!

故意跟上游不一致的地方(有意为之,不是忘了):

- **Wi-Fi 默认关**(风暴吓的,见上)
- **RGB LED 默认关**(摄像头受不了)
- **zram 12 MiB**,默认 zram-only(卡上 p3 swap 机制保留)
- **宿主构建在 Linux x64 原生验证**(Ubuntu 24.04);上游的
  macOS/container 流程代码还在,但这个 fork 没练过

## 构建与烧录

```sh
# 一条命令出权威镜像(Linux x64,冷缓存约 25 分钟,96 核)
scripts/soft-icewm-native-build.sh

# 产物写卡(SHA-256 回读校验)
sudo scripts/flash-sd-linux.sh /dev/sdX build/artifacts/sdcard-soft-icewm.img
```

串口 115200,控制台 root 免密,SSH 只收 key。加载器、内存布局、
flash 分区、内核补丁集这些没变的硬知识,上游 README 写得怪好:
[docs/README-upstream.md](docs/README-upstream.md)。

## 路线图

- [ ] 合并上游 v0.7.x(原生 I2C、新板子、tmux 桌面;触摸换硬件 I2C
      后,bitbang 轮询那点 CPU 开销也能省下)
- [ ] ilp32f(硬件 F ABI)全量重建,调研和烟雾测试已过
- [ ] dwc2 对特定 USB 设备的 split 错误重试风暴(0014 之外还得再来
      一剂)
- [ ] Wi-Fi auth timeout 根因(上游也没修,难兄难弟)

## License

MIT,同上游(`LICENSE`);各组件许可证列表见
[docs/README-upstream.md](docs/README-upstream.md) 结尾。
