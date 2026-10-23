# 实战笔记:坑、修复、技巧、设计决策

把把这个 fork 撑起来一路撞上的东西全记在这。英文原版是
[notes.md](notes.md),两边内容一致。发现写错了欢迎指正。

## Wi-Fi auth 风暴的三个藏身处

ESP-Hosted 固件 + 宿主驱动对连不上的 AP 会**永远重试 auth**。在
10.6 MiB 的系统上,重试的代价把别的活全饿死:开机约 8 分钟后串口死掉
(内核回显还在,shell 永不回答),连 X 一起。这个风暴我们撞见三次,
每次躲在一份不同的 `wpa_supplicant.conf` 后面:

1. **Buildroot 包默认的** `/etc/wpa_supplicant.conf` 带一个开放网络块
   (`key_mgmt=NONE`):wpa 见到任何开放 AP 都扑上去重试。它躲在
   overlay 副本底下,你不删 overlay 它就不走。
2. **SD p1 上带真实凭据的模板**。`S40wifi` 优先用 p1 上的 conf 而不是
   rootfs 的,所以"我们删了 rootfs 的 conf"在 p1 还有备份时等于没修。
   第一次修复就是这么被穿裆的。
3. **rootfs 副本本身**,运行时被用户加进网络块的那种。

所以这个 fork **一份 conf 都不带**,`S40wifi` 没 conf 时干净失败。
Wi-Fi 改 opt-in:`wpa_passphrase ssid pass > /mnt/sd/wpa_supplicant.conf`
然后重启。p1 上的 `NOX`、`NOSWAP`、`NOWIFI`、`NOWALLPAPER`、`RGB` 是
同一族开关。

怀疑中招时的诊断:控制台冻住但内核还在回显;`dmesg` 里刷
`auth timeout` / `Failed to send assoc`。冻住之后唯一的解法是断电。

## kswapd 回收活锁:14 MB 工作集塞 10.6 MiB

Linux 只分到 16 MiB PSRAM 里的 10.6 MiB(内核约 5 MB、OpenSBI
0.35 MB、framebuffer 0.75 MB)。X 桌面的工作集约 14 MB。内存见底时的
死法**不是 OOM killer**,是这个:

1. 匿名页塞满 zram(12 MiB 上限,lz4,约 3 倍压缩);
2. kswapd 开始回收 page cache——**包括所有在跑程序的代码页**;
3. 那些页从 SD 卡换页回来,把别的页挤走;
4. load 爬过 5、iowait 到 40%、idle 归零、控制台几分钟才应一句——
   **而每个进程都还活着,`dmesg` 干干净净**。

修复,按杠杆排序:

- **zram 12 MiB**(`S05swap.sh`):不用不花钱(pool 按需增长),压缩
  约 3 倍,把*匿名*压力从 page cache 上挪走。
- **xterm 只在有 USB 键盘时启动**(`startde`):省 ~1.5 MB RSS。
- 守护进程静默化:`post-build.sh` 不让 dropbear/bluetoothd/dbus
  自启(合计约 3 MB 工作集)。

定位这个病的账本法:`free` 的 `used` 远大于 RSS 之和——差值就是 swap
压力和内核 slab。`ps` 按 RSS 排 + `grep '^cpu ' /proc/stat` 跑两次
(盯着 `system`、`iowait` 涨而 `idle` 不动),两分钟出诊断。

## DWC2 USB 设备风暴

某个特定的 USB 设备(插在板子上那个)把 DWC2 host 控制器搞死了:
`grep dwc2 /proc/interrupts` 以约 8k IRQ/s 爬升,日志刷
`usb 1-1: clear tt 1 (...) error -71`(经 hub 的 split 事务失败),
系统在内核态饱和(`system` 75%+、`idle` 0)。拔掉它,板子跑着跑着
就活过来了。Patch `0014` 砍掉了 SOF 调度的开销,但盖不住这条错误
路径——配套补丁在路线图上。

阴险的地方:症状是黑屏 + 冻住的光标,跟上面的回收活锁**长得一样**。
先看 `/proc/interrupts`,再谈理论。

## 静默的 flash 内核回落

loader 优先用卡 p1 的 `IMAGE.BIN`;**读卡失败就静默回落 flash 里的
内核**。失败很罕见(实测一次:`SD mount failed: ESP_ERR_TIMEOUT`),
而且两种 boot 从登录提示看一模一样。我们就是因此对着错误的内核测出
了"FPU 回归"和"isa=rv32imac"。

规矩:每次要认真对待的 boot,查一眼构建主机和日期:

```sh
dmesg | grep version
```

`kail@kail-epyc ... Oct 23` 是卡上的内核;其它都是 flash 的。重刷
loader 之后这个回落就不存在了。

## F-only FPU:设计与未解的回归

这颗 hart 是 `rv32imafc`——有 F 没 D。主线 riscv 内核拒绝这种组合
(validate 钩子 + elf_hwcap 掩码都会把 F 扔掉)。Patch
`0017-riscv-f-only-fpu.patch`:

- `fpu.S`:上下文切换用 `fsw/flw`(32 位存取)替代 `fsd/fl`;
- `cpufeature.c`:validate 钩子接受 F-only;elf_hwcap 掩码删除;
- `switch_to.h`:`has_fpu()` 改为看 `RISCV_ISA_EXT_F`;
- 内核 C 代码保持软浮点(`KBUILD_BASE_ISA` 不含 f/d),内核里唯一的
  浮点指令就是这段手写切换。

用户态无论何时都是软浮点 `ilp32`,老二进制不碰 FPU;想显式用 F 指令
的程序现在能用了。`/usr/bin/fputest` 跑算术 + 并发检查,打印
`FPUTEST_PASS`。

**未解问题**:在合并的 v0.7.1 基座上内核又报 `rv32imac`。已验证:
patch hunk 都在编译树里、`riscv_ext_f_validate` 反汇编是"直接返回
0"、CONFIG_FPU=y、DTB 的 `riscv,isa-extensions` 里有 `"f"`——但
resolve 出来的 bitmap 把它丢了。丢失发生在 resolve 管线的别处,
继续挖。

## EXA:加速了什么,为什么 Composite 不挂

| EXA 操作 | 引擎 | 实测 |
| --- | --- | --- |
| Solid | PPA fill | ~213 MB/s,全屏 89 fps |
| Copy | DMA2D M2M blit | ~106 MB/s,滚动 256 fps |
| Composite | 不挂 | 见下 |

fbdev 参考:15.7 / 9.1 fps。S31 的 Xorg 驱动在
`br2-external/package/xdriver-xf86-video-s31/src/`(EXA PrepareSolid/
Copy → PPA/DMA2D 寄存器序列;重叠 Copy 会回落到安全方向)。

Composite 故意不加速:PPA blend 引擎用**固定 per-plane alpha** 合成
两个 framebuffer,而没有合成器的桌面发出的 Render 全是"纯色 × a8 逐
像素 mask"(抗锯齿字形)。RGB565 pixmap 不带 alpha,引擎也只能寻址
framebuffer 物理范围,RAM 里的 mask pixmap 够不着。挂上
PrepareComposite 只会买到诊断,买不到帧。

通道纪律:JPEG 编解码(`jpegdec`/`jpegenc`)也用 DMA2D 通道 0——
它们保存/恢复 staging 寄存器,且绝不与 X 并发。

## icewm + cmake ≥ 3.28:装出 target 边界

icewm 的 po 机制用**绝对** DESTINATION 装 `.mo`(`po/playground`),
老 cmake 在 `DESTDIR` 下会无视它,cmake 3.28 会给它垫前缀——文件落进
Buildroot 的 target,扫描器当场拒收。`-DSKIP_TRANSLATIONS=ON` 把整个
po 处理关掉(反正我们只要英文)。任何带翻译的 CMake 包都要防这个
套路。

## 内核体积陷阱:48 个 NET_VENDOR_* 家族

给 Function-CoreBoard-1 开 `CONFIG_ETHERNET=y` 之后,`olddefconfig`
会把**每一个** `NET_VENDOR_*` 家族默认成 y——48 个 x86 时代的厂商
家族编进了 RV32 内核。在这颗 SoC 上它们大多是空壳(依赖它没有的
PCI/IO 口),所以关掉的体积收益不大——但任何对体积敏感的内核都值得
做这个卫生,`linux.config` 现在显式钉死它们。
`scripts/slim-kernel-rebuild.sh` 是这类改动的增量重编脚本。

## 内存地图与部署模型

| 范围 | 用途 |
| --- | --- |
| `0x50000000` | 内核镜像(常驻,约 5 MB) |
| 中间 | **10.6 MiB 可用 RAM**(`free` 的 total) |
| `0x50f00000` | OpenSBI(0.35 MB) |
| `0x50f40000` | framebuffer(0.75 MiB,uncached) |

部署:**内核在卡里**(p1 的 `IMAGE.BIN`,loader 优先于 flash 引导),
写一次卡就是全系统升级。SPI flash(loader/OpenSBI/radio 固件)刷
一次就行——上游 release 的 `flash.bin` 直接能用——只有 loader 本身
变化时才需要重刷。

## 屡试不爽的技巧

- **无摄像头截 framebuffer**:
  `dd if=/dev/fb0 bs=1600 count=480 | gzip | base64 -w 76`,文本走
  串口拉回,md5 校验,然后
  `ffmpeg -f rawvideo -pix_fmt rgb565le -s 800x480` → PNG。768 KB 的
  framebuffer 压完约 9 KB;串口只在系统空闲时传得动——X 一跑就丢字节
  (UART FIFO 溢出)。
- **诊断写卡,不伺候串口**(`S70diag`):每次开机采 15 分钟
  load/cpu/mem/swap,外加 8 MB SD 读速测试,写进 p1 的 `diag.txt`。
  串口可以冻,卡会说话。
- **任何"板子冻了"的理论之前**先 `grep dwc2 /proc/interrupts`;
  **任何跑分之前**先 `dmesg | grep version`。
- **摄像头拍板**:BRIO 手动对焦 75(`focus_absolute` 越大越*远`),
  ffmpeg 拉 1280x720 MJPG,喂 30 帧等收敛。
- **串口复位**:带 DTR/RTS 打开串口就会复位板子——控制台守护就是
  利用这个;脚本只想读串口时小心这个坑。
- **别在命令行含 `<目标串>` 的 shell 里 `pkill -f <目标串>`**——会
  匹配到自己的命令行把自己杀掉。释放串口用 `fuser /dev/ttyUSB0`,
  靠谱。
- **敏感网络推 GitHub**:大推送死于 GnuTLS -110;`git -c
  http.proxy=... push` 走本地代理就通。另外**浅克隆不能 push**(远端
  `unpack failed: index-pack failed`):先 `git fetch origin
  --unshallow`。
- **`dl/` 权限反复横跳**:容器/root 和普通用户混跑之后,Buildroot 会
  在下载深处报一句光秃秃的 Permission denied——`chown -R` 缓存目录。
