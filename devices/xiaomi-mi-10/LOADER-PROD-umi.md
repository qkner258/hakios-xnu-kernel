# UMI production loader（仅本地实现，尚未上机）

2026-11-11。目标是无运行时限地运行 iOS；这是保留 UEFI 服务核的生产策略版本，不是已经完成认证的整机生产固件。本次没有访问手机，没有运行 adb/fastboot/ssh，没有刷写或运行 userland payload 全量构建。

## 选择：同源双产物

保留 `devices/xiaomi-mi-10/loader/loader.c`，生产版增加 `-DHAKIOS_LOADER_PROD`，产物为 `out/xiaomi-mi-10/flash/loader-prod.{obj,efi}`。日用命令和 `loader.{obj,efi}` 的编译/链接参数保持原样；`all` 仍只构建 debug loader。避免复制约 190 KB 的 loader 后 UFS、充电、白名单和内存映射实现漂移。

生产开关只控制 loader；内核使用同一个二进制，在 debug loader 下仍走原有行为。内核生产支持由设备补丁与 `overlays/xnu/files/osfmk/arm64/umi_loader_prod.inc` 提供，未修改公共内核模板、kern_minimac、Metal/GLES 或 hotspot 源码。

## 原有 debug 路径（调研结果）

- `phone-test.sh` 经设备 `run.sh` / runtime `xl` 启动。`xl enter` 将 loader 包成 `--usb` 的 XNUP，再经 UEFI 镜像临时启动；resident ramdisk 和 kernel payload 后续 staging。`xl run` 无条件注入 `loader_return_after=${XL_SECS:-8}`，并有自己的 host deadline/recover 逻辑；`phone-test.sh` 还设 `XL_REBOOT_AFTER=1`。
- `loader.c` 的 `kernel_budget` 来自该参数。`run_on_other_cpu()` 用 PSCI CPU_ON 在通常为 MPIDR 0x700 的 CPU7 运行 XNU，CPU0 留在 UEFI。CPU0 轮询 UFS mailbox、充电缓存、live log/heartbeat 和 USB。约 24 秒后屏蔽本核 IRQ/FIQ 和 GIC group1；UFS 和充电轮询仍继续。
- debug watcher 喂狗到 `budget+60`；USB handoff 模式另有独立 `budget+20` 的 `usb_kernel_reset()`。50 秒无 heartbeat 时原路径读出挂起内核的日志并返回。**另一个实际细节：原 `wdt_arm()` 只接受 platform_redwood，UMI 并未因此启用该虚拟看门狗。此处只在生产分支为 UMI 启用。**
- 内核 `loader_return()` 停其它核、quiesce 设备、整理开头 256 KiB＋最近 1 MiB 的日志环及 msgbuf/panic 文本，经 `loader_stub_slot` 返回。CPU7 的 `other_cpu_return_stub` 把日志位置交给 CPU0 后 CPU_OFF；同核 stub 则恢复 UEFI 寄存器/栈。loader 恢复 UFS、取日志、再次响应 host。
- `hakios.input.c` 的五指按住 1500 ms 会调用 `loader_return()`；内核 deadline 和 host stop 也会走它。设备 `xnu-device.patch` 已在 `reboot_kernel()` 提前关闭 HFS 日志文件、sync/unmount、停止驱动并调用 `loader_return()`，因此 iOS 的关机和重启原来都结束 debug run。`halt_all_cpus()` 另有 PEHaltRestart 无效后的返回路径。最终是 host/loader 的调试流程重启 Android，并非 iOS 真正 SYSTEM_OFF。

## 生产行为

### 协议与无时限

不新增任何 iOS boot-arg。复用现有 `XNULDRST` 四字槽：`slot[1]=0`（没有返回 stub），`slot[2]=0x50524f4400000001` 表示 production ABI1，`slot[3]` 仍为 PA−VA。debug loader 仍写原 checkpoint。内核提供对齐的 `{0x31444f5250494d55, 1}`（`UMIPROD1`）能力记录；生产 loader 在加载后的 kernel 中确认能力、live block 和返回槽，拒绝旧内核。

生产版要求恰好一个 `bootcpu=7` 和一个 `cpus=7`、成功启用 watchdog、无 checkpoint；拒绝 `loader_return_after=`、`usb=`、`usb_ncm=` 和被截断的最终命令行。原 whitelist 和分区写入限制保留。607 字符上限仍有效；mode 不消耗字符，也不注入 `hv_wdt_pet=1`。

生产内核不注册 deadline/host-stop timer，不注册五指 exit callback。生产 watcher 不执行 live USB 命令、不写 host 日志、不执行预算复位。没有总运行时长；CPU0 的 UFS/充电 broker 循环可无限运行。CPU 启动失败、早期异常或不可能的 kernel return 都停止喂狗，不恢复 debug menu。

### 关机与重启

正常 `reboot(2)` 保留原有 HFS logger 最终快照、文件关闭、sync/unmount 和驱动 stop 顺序，然后把 `RB_HALT` 转成 SYSTEM_OFF，其余转成 SYSTEM_RESET。`halt_all_cpus(reboot)` 也在调用 PEHaltRestart 前进入生产处理。

`loader_prod_halt()` 先原子发布关机意图，再经 `loader_return()` 的原有 stop_others/quiesce 路径转到 kernel boot core。在日志整理后直接 SMC 调用 PSCI `0x84000008` / `0x84000009`；UMI 的平台描述使用 SMC。不会跳回 UEFI stub。panic、热/电池保护或其它意外的 `loader_return()` 默认 SYSTEM_RESET，不会落回 loader。若 PSCI 错误返回则屏蔽中断停住，依靠 watchdog 复位。早期异常时 slot[1] 为零的汇编等待同样由 watchdog 收尾。

SYSTEM_RESET 的下一个系统由现有启动链决定，现在可能是 Android/fastboot；这属于显式 reboot 或故障复位，不存在正常运行 N 秒后自动回 Android。不能承诺当前启动链会自动再次选择 iOS。

### Watchdog：CPU0 根据内核心跳喂狗

选择 watcher 喂狗，而不是让内核喂：现有代码明确尚未确认其它核的 PET 是否计入 CPU0 的虚拟 watchdog，同时 CPU0 本就必须提供存储服务。

生产 `wdt_arm()` 为 UMI 调用现有 Haven SMC SET_TIME/CONTROL/PET，三者必须成功才允许启动 XNU。handoff 后 bite 设为 60 秒。内核仍每 10 秒推进 live heartbeat，但生产模式强制禁用内核自己的 PET。CPU0 用 acquire load 读取心跳：新心跳后仅 25 秒内可每 5 秒 PET；首次心跳有至多 50 秒启动宽限。宽限是故障检测，不是运行预算。停止推进后，最后一次 PET 约在停止后 25 秒内，随后最多约 60 秒 bite；启动时无心跳通常在约 110 秒内 bite。CPU0 卡住也无人 PET。

硬件实际 SMC 支持、bite 延迟和复位去向尚未验证。心跳证明 scheduler/timer 工作，不能识别“UI/GPU 死锁但 heartbeat 仍正常”的局部故障。将来系统 suspend 必须设计 watchdog 的 suspend/resume 所有权，不能直接停 heartbeat。

### 为什么保留 CPU0 watcher

`NIGHT-LOG.md` 的 final8core（EIGHT=1、bootcpu=0 cpus=8）证明曾进入 SpringBoard 并按时返回；后续记录也提醒该夜间回归未核对屏幕。它没有证明无限运行、充电状态更新、watchdog 或关机都可靠。

目前 `ufs=2 ufs_native=0` 的读写依赖 CPU0 的 UEFI Block I/O mailbox，`battery=1` 的缓存依赖 Qualcomm UEFI ChargerEx/QcomCharger 轮询。直接占用 CPU0/全部八核会失去这些服务；ExitBootServices 还需要真正移交所有权。最小可行方案因此仍是 CPU7 boot、7 个 XNU 核、CPU0 supervisor。代价是占一核、常驻轮询及 UEFI 内存，并不是原生省电/深睡眠方案。

生产 CPU0 从 handoff 起屏蔽本核中断，避免 UEFI timer/USB callbacks 在 XNU 的 GIC 配置上运行；仍保留同步轮询 UFS 与每 5 秒充电查询，且只有 UEFI transfer list 在位时查询充电。USB gadget/NCM/usbmux 关闭；保留启动前的 firmware fastboot staging。真正八核生产启动需要 native UFS、独占 PMIC/充电与温度保护、USB 所有权移交、kernel 喂狗及 suspend 支持，不能只改 cpus 参数。

### 日志落盘

复用现有 `devices/xiaomi-mi-10/drivers/umi/hakios.usb.c` 的独立 HFS 文件线程。生产模式仅启动该线程，不启动 USB worker/USB MMIO/handoff。需要 `ufs_write=1 var_hfs=ufs_darwin_data`，只在真实 `/dev/ufs_darwin_data` 的 HFS 挂载后写普通文件：

`/private/var/db/hakios-usb-<epoch>-{0,1}.bin`

保留历史文件名以兼容 `tools/read-usb-hfs.py`。两槽交替保存 kernel log head＋tail ring，header/trailer、FNV checksum 和 fsync 检测撕裂。生产每 60 秒保存（debug 仍是 2 秒），正常关机提前唤醒写最后一份并关闭 vnode，之后才 sync/unmount。无 host epoch 时生产线程读取 kernel calendar time（loader 的 firmware boot-epoch 提供初始时间）。既有 logd 的 `/var/db/diagnostics`、`uuidtext` 也继续使用 HFS `/var`。

限制：线程启动/打开文件/I/O 失败仅记错误，尚不阻止启动；panic/reset 最多丢最近约一分钟，最后一次快照之后的 quiesce/panic 文本可能仅在 RAM；RAM ring 不保证跨断电保留。每槽上限约 1.25 MiB，每次 boot 新建两槽，无跨 boot 自动清理；持续写入约 1.8 GiB/日上限，后续应增加脏数据检测/轮转。epoch 重复时 O_EXCL 拒绝覆盖旧日志，可能没有新快照。只有普通 HFS 文件写入，没有 raw var_store、GPT 或其它分区写入。

## 构建与产物

从 `work/umi-v23`：

```sh
bash devices/xiaomi-mi-10/flash/build.sh loader-prod
# 日用命令仍是：bash devices/xiaomi-mi-10/flash/build.sh loader
```

配套内核也必须重新编译；单换 loader 无法关闭旧内核五指退出/实现真实关机。本次已经编译并单独复制为 `out/xiaomi-mi-10/flash/kernel-prod`。未重建/替换 APFS payload；它们在 staging 时由 mkpayload 与 kernel 组合。

可重复编译内核（host only，使用项目 build lock；不构建 userland）：

```sh
source /Users/xm666/X/umi-ios/env.sh
/Users/xm666/X/umi-ios/build-lock.sh bash devices/xiaomi-mi-10/build-kernel.sh -j8 BUILD_DSYM=0
cp out/xiaomi-mi-10/xnu/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837 out/xiaomi-mi-10/flash/kernel-prod
```

首次正常内核构建在 dSYM 的 shell process substitution `/dev/fd` 被沙箱拒绝；随后以受支持的 `BUILD_DSYM=0` 成功完成编译、链接、strip、symbol-set 检查。所有生产 hook、input 和 USB logger 对象已编译链接；成品内核恰好包含一个 ABI1 capability、return slot 和 live block。

### debug 不变证明

`out/xiaomi-mi-10/flash/prod-proof/` 保存本次修改前的日用 EFI、baseline 源码/对象/EFI，以及修改后重建对象/EFI与构建日志。

ARM64 COFF `.obj` 的偏移 4..7 是时间戳，PE 的 COFF header 时间戳同样会改变。**除这唯一四字字段外，修改前后所有字节相同**（包含代码、重定位、常量和布局）；原日用 EFI 与重建 EFI 也满足这一比较。验证脚本：

```sh
python3 devices/xiaomi-mi-10/tools/compare-loader-code.py out/xiaomi-mi-10/flash/prod-proof/debug-before.obj out/xiaomi-mi-10/flash/prod-proof/debug-after.obj
python3 devices/xiaomi-mi-10/tools/compare-loader-code.py out/xiaomi-mi-10/flash/prod-proof/debug-daily.efi out/xiaomi-mi-10/flash/prod-proof/debug-after.efi
```

时间戳归零后的对象 SHA256：`a0ccbe3d388f834d751da0fb4630418ea5fe6eae67471a3cf1c67b7185d8d8bd`。
时间戳归零后的 EFI SHA256：`3e85b3d830c3a09bf84857bc66303265da9826fbe14888f4a138926a5d268301`。

debug 编译/链接不保证字节级可复现，原因是这些时间戳；本次完成后已恢复原始日用 `loader.efi`，故最终 SHA256 也与开始时完全相同。生产链接单独设 `/timestamp:0`，两次本地重建 EFI 完全相同。

最终 SHA256：

- `loader.efi`：`b60bc14083f707a6326cdfac4d0e1608c8d07b1899a793f10dbe24a6e7ac202d`
- `loader-prod.efi`：`7a598db937d07a0e22d90e7b5dbdf776fc83657dff082faeeba82cb172ca2281`
- `kernel-prod`：`db0763fcc59c5757b28bce2c81062288eecd990b8c8820ef79d459edd676d58f`

此外已将实际 loader 参数 gate 提取到临时 host harness 编译运行：debug 接受原 timeout/USB 参数，production 拒绝；两者均拒绝 raw var_store 和未知参数。硬件行为不在这些本地检查的覆盖范围内。

## supervisor 将来如何 staging 测试（本次未执行）

**不能用原 `phone-test.sh` 或 `xl run`。** 它们会注入 deadline、启动 host recovery，并在结束后发送 Android reboot；production 明确拒绝 deadline。不能只设 `XL_SECS=0` 或清空 XL_REBOOT_AFTER 便认为已关闭整个 host 流程。

已准备 `out/xiaomi-mi-10/flash/loader-prod-stage.img`（SHA256 `ade35cd1d44feda63b15b9595543980cfc50c35000ee1ea10a03cc35d1f24d79`），其中是 production EFI＋已知 UMI mm-vote UEFI，启动前提供 firmware fastboot staging。另已生成 38,655,440 字节的 GL `run-prod-payload.bin`，使用本次 `kernel-prod`＋现有 GL root；用户参数 336 字符，原文保存在本地 `prod-proof/staging-args.txt`。其中 epoch 是打包时刻，将来测试应更新 epoch 并重新核对当日配置。host 上重建该暂存镜像（不接触手机）：

```sh
source /Users/xm666/X/umi-ios/env.sh
F="$PWD/out/xiaomi-mi-10/flash"
export UEFI_FD=/Users/xm666/X/umi-ios/ios-port-research/repos/JopangJovan_hackios-xnu-kernel-umi/work/display-uefi/variants2/SILICIUM_UEFI.mm-vote.fd
python3 xnuloader/mkpayload.py -o "$F/usb-prod-payload.bin" loader.efi="$F/loader-prod.efi" --usb
python3 devices/xiaomi-mi-10/tools/mkimage.py -p "$F/usb-prod-payload.bin" -o "$F/loader-prod-stage.img"
```

supervisor 应先核对当日 payload/驱动组合，构造单独 `run-prod-payload.bin`，kernel 使用 `kernel-prod`，ramdisk 使用当日匹配的 `root-flash-ui.apfs`（GL）或 CA 版本；命令行沿用已验证完整驱动集，必须含 `bootcpu=7 cpus=7 ufs=2 ufs_native=0 ufs_write=1 var_hfs=ufs_darwin_data battery=1`，加既有 epoch/Wi-Fi MAC 配置，去掉 timeout/USB/checkpoint。最终全部参数须 ≤607 字符（含 loader 前缀）。示例 host 打包：

```sh
# PROD_ARGS 由 supervisor 设置为完整、已核对的当日配置，不能只填上述必需参数。
python3 xnuloader/mkpayload.py -o "$F/run-prod-payload.bin" kernel="$F/kernel-prod" ramdisk="$F/root-flash-ui.apfs" --cmdline "$PROD_ARGS"
```

仅由 supervisor 在独占 phone lock、手机已处于 stock fastboot 时手动进行以下 RAM staging；这不是 flash/install 命令，也不要放进自动重试/恢复脚本：

```text
fastboot boot <F>/loader-prod-stage.img
# 等待 production loader 枚举；核对屏幕 UMI PRODUCTION ABI1 标记。
fastboot stage <F>/flashboot.apfs
fastboot oem ramdisk
fastboot stage <F>/run-prod-payload.bin
fastboot oem run
# 到此停止 host 命令；XNU 运行后不再响应 firmware fastboot。
```

先验证能启动、五指不会退出，等待超过原 debug SECS 及 +60 秒仍正常；分别测试 iOS 滑动关机（真正断电）、iOS reboot（全机重启）、新 boot 的 HFS 日志。另一次受控测试停止 kernel heartbeat，验证 CPU0 不再 PET，hardware bite 复位；不要关闭热/电池保护，也不要把仅挂 UI 的局部故障当成 heartbeat 测试。

**本版本不会按时返回 Android。** 正常退出使用 iOS“关机”；卡住时人在设备旁长按电源触发 PMIC 硬复位，具体长按时长应以这台 UMI 的实际行为为准，必要时电源＋音量下进入 stock fastboot。硬复位可能丢未提交 HFS 数据，优先正常关机。不要依赖五指、`xl stop`、host USB 日志或 N 秒回 Android。SYSTEM_OFF 在接线充电时是否保持关机/转充电界面，也须实测。

## 无 Mac 启动的将来方案（仅文档）

现有 loader 已能从其 LoadedImage 所在的 EFI SimpleFileSystem 卷读取 `\\EFI\\hakios\\payload.bin`。将来可由 UEFI Boot#### entry 或 dual-boot 菜单显式选择 `loader-prod.efi`，同卷 payload 带 kernel、短命令行及 resident/ramdisk 项，Apple root/HFS `/var` 继续在现有 darwin 用户分区。需核实该 UEFI 是否持久保存 Boot####、是否可读取所选文件系统、resident/payload 内存与大小限制、RTC/epoch、失败回退和物理按键选择。

Stock Android 启动链目前不会凭空进入这套 UEFI。任何永久入口、放置文件的分区/布局变动、UEFI 安装/启动链变更，都需要用户事先明确同意并单独评审；本任务不安装启动项、不刷任何分区、不写 GPT，不改 dtbo、modemst/fsg/persist/EFS。也不提供自动刷写流程。

## 本次修改文件

- `loader/loader.c`、`flash/prepare.py`：同源 production 构建及运行策略。
- `xnu-device.patch`、`overlays/xnu/files/osfmk/arm64/umi_loader_prod.inc`：设备级内核协议、shutdown/timer/watchdog hooks。
- `overlays/xnu/files/bsd/dev/hakios.input.c`：production 禁用五指退出。
- `drivers/umi/hakios.usb.c`：production 仅启动已有 HFS logger、calendar epoch fallback、一分钟快照间隔。
- `tools/compare-loader-code.py`：逐字节比较 debug 对象/EFI，只排除时间戳。
- 本文档。路径均相对于 `devices/xiaomi-mi-10`；`out/` 只有本地构建产物与验证记录。

## 尚未解决的上线风险

- 必须实测 Haven watchdog SMC 在 SM8250 的响应与真实复位；失败时生产 loader 会拒绝启动，不是退化成无 watchdog 无限运行。
- PSCI OFF/RESET、其它核退出、DSP/GPU/UFS quiesce 与拔线/接线状态均未上机验证，固件调用或 teardown 挂起会变成 watchdog reset。
- UEFI charger 查询在 IRQ 屏蔽后的长期行为、温度新鲜度、过温/低电量保护及长期充电仍需验证，参考根目录 CHARGE-umi.md；不添加 PMIC 盲写。
- 七核 watcher 模式仍耗电发热，深睡眠和完整 kernel 硬件所有权尚未实现。心跳继续时的局部死锁无法自动检测。
- HFS 非正常复位恢复、持续日志写放大、跨 boot 日志清理和有效 RTC/epoch 仍需完善。现有两槽只检测快照撕裂，不代表整个 HFS 在掉电后一定无损。
