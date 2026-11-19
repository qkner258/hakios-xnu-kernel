# 全开 Metal（`metal_all=1`）试验清单 — 2026-11-10

试验：v6 组件（k-v6 1be5d3b3、loader-v6 eb390212、root-flash-ui-ca.apfs.v6 2ec6e424）+ `metal_all=1`，SECS=900。
无 panic，到点自动回 Android。

日志：
- `logs/test-1110-095629-umi-metal-all.{out,kernel.log,live.log}`
- `logs/test-1110-095629-umi-metal-all.union.log`：412 份 live 快照去重合并，已滤掉 commcenter 刷屏

## 总结

**比预想的好得多。**

- 14 个进程拿到了 GPU。
- **shader 编译 0 失败**，覆盖：
  - 相机后台 mediaserverd 的 CNN/深度滤波 compute shader，约 50 个；
  - 地图 VectorKit、备忘录 PencilKit、Widget（chronod）；
  - 壁纸（PosterBoard / CollectionsPoster）里的 compute 和 render shader。
- 没有哪个进程因为 Metal 崩溃。

真正的问题集中在三类：

1. 少数**还没实现的 API 或功能**，驱动拒绝执行，对应画面就是黑块；
2. **GPU 页错误后整机画面卡死**，最严重；
3. **新分配的显存没清零**，没画的地方会露出旧内存（开机 Apple logo）。

## 清单（按优先级）

| # | 现象（用户观察） | 进程 | 日志证据 | 原因 | 工作量 |
|---|---|---|---|---|---|
| 1 | 用了一会儿后**整机画面卡死**（10:07:20，刚从"家庭"App 退出） | backboardd 及全部 | `THE SMMU REFUSED the 3D core: address 0x24980000`；随后 `gpu: no answer`，每个命令 2.4–5 s 超时 | GPU 读了一块已被释放或解除映射的内存。全开后 App 也在用 GPU 画 IOSurface，**跨进程的缓冲区生命周期没管好**，App 释放时 GPU 还在用。另外 `gpu_recover` 默认关闭，一挂就恢复不了 | **中～大**：buffer/IOSurface 引用计数要等 GPU 完成再释放，再加上 GPU 挂起后的自动恢复。**全开前必须修** |
| 2 | 地图、备忘录手绘：**一闪黑屏加 Apple logo**，上滑回桌面恢复 | Maps、MobileNotes | 绘制被拒（见 #4）；整轮没有 GPU 故障 | 被拒或还没画的 surface 内容是**未清零的旧物理内存**，正好是 UEFI 开机画面 | **小**：新分配的 surface/纹理清零（或首帧 clear）。同时有安全意义，不该把别的内存内容露给 App |
| 3 | 桌面壁纸、设置→墙纸：**很多黑块** | PosterBoard | `refused: a draw of downsample_blur...: texture 3 was made without shader-read usage` | 苹果 GPU 对 usage 标志很宽松，我们太严格。Adreno 可以采样任何纹理 | **很小**：放宽这条检查 |
| 4 | **地图显示黑色** | Maps、MapKit.SnapshotService | ① `multisample Depth32Float_Stencil8 texture` 被拒；② `a depth test ... with no depth attachment` 拒绝 Grid/Clear 绘制；③ 快照全是 `(0,0,0,0)` | 还不支持 **MSAA 和深度/模板附件** | **中**：渲染路径要加深度/模板缓冲和多重采样 resolve。这是 3D 和游戏的基础，一次做好收益很大 |
| 5 | 无边记/备忘录画图：**笔迹在画的过程中部分闪烁**，松手后正常 | MobileNotes（PencilKit） | 备忘录 fps 平均 9.8；PencilKit 的 pipelinelib 没有 Adreno 版本，现场编译了 14 个 | 推测是实时笔迹层用了 enqueue/present 顺序，或部分 surface 更新时序有问题。日志里 `command buffers run in the order they are COMMITTED, enqueue not kept` 是嫌疑之一 | **中**：需要专门排查 |
| 6 | 动态壁纸/控制中心某些视频帧 | SpringBoard | `IOSurface N has 2 planes: a single-plane surface is what is served`（420f YCbCr） | App 侧还不支持双平面 YCbCr 纹理。backboardd 侧已有 GPU YCbCr 转换（video-gpu 新交付），可以复用 | **小～中** |
| 7 | 照片 | MobileSlideShow | `supportsSampleCount: is not implemented`（返回 0） | 漏实现的查询接口 | **很小** |
| 8 | （无可见现象） | backboardd | `reads 64 bytes of constant buffer 3; 16 are bound` 两次被拒 | 绑定的常量缓冲比 shader 读取的短。苹果驱动容忍，我们拒绝 | **小**：越界部分补零，不再拒绝 |
| 9 | （性能） | chronod（Widget） | 首次编译：compiler wait 428 ms，compile 1610 ms | 冷启动编译慢，之后走磁盘缓存 | 已有缓存；可以预热 |

## 和 Metal 无关、顺手记录的问题（全开之前就存在）

| 现象 | 证据/说明 |
|---|---|
| **Safari 历史记录重启后消失** | 本轮 `com.apple.Safari.History` 连续 3 次 SIGABRT，启动后 0.25 s 内就崩，和 GPU 无关；用户确认以前就有。另开任务排查（可能是数据库路径或权限问题） |
| **桌面状态栏不显示充电闪电**，百分比偏左（闪电的位置空着）；进入 App 后才显示 | 桌面状态栏的充电符号缺失，App 内状态栏正常。以前就有，属于电池/状态栏任务 |
| **后台在空转**：SpringBoard 和 vmd 每秒约 2000 次查找 `com.apple.commcenter.xpc` | 所有轮次都有（v6、USB 轮都是每秒 2000+），launchd 一直在打"Last log repeated 2xxx times"。CommCenter 没有运行（基带还没通），客户端在忙等重连。**很可能是卡顿和发热的来源之一**，交给 fps 或基带任务：要么给一个最小的 CommCenter 桩，要么拦住这个重试循环 |

## 建议顺序

1. 先做 **#1（GPU 页错误和恢复）+ #2（显存清零）**，它们决定全开是否安全；
2. 再做 **#3、#7、#8**（小改，壁纸黑块马上消失）；
3. 然后做 **#4（深度/MSAA）**，地图和 3D 依赖它；
4. 最后做 **#5、#6**。

全部完成后，`metal_all=1` 就可以作为默认。在那之前保持名单制（backboardd、SBRendererService、PosterBoard）。


## Fix #2/#3/#7/#8 — umi-metal-small（2026-11-10）

只做代码、主机测试及构建；未接触手机，未增加 boot-args。需要同时使用本节的 kernel 和 CA payload。

- **#2**：核对新页路径为 `IOBufferMemoryDescriptor → IOMallocAligned_internal(Z_ZERO) → kernel_memory_allocate(KMA_ZERO)`，包括独立 GPU buffer/texture 和新 IOSurface，已有页级清零，不再重复 memset。实际缺口是 `HakiosGPUUserClient::keep()`：旧默认 `gpu_keep=1` 把内容原样留给下一次分配。现在只在 GPU fence 完成后、归还内核池时清零整块；cached 映射 clean 到内存，write-combined 映射执行 barrier。`gemNew()` 池命中不再清零，直接取已清零的块。IOSurface 保留已有的后台/帧间预清零及 clean 标志；移除 `surf_clear=0` 绕过清零的测试开关，尚未预清零的块仍在交付前清零。导入现有 IOSurface、包装调用者自己的内存保留其内容，不当作新分配清空。
- **#3**：删除 render 路径的 ShaderRead usage 拒绝条件，适用于 VS/FS 采样；纹理形状及元素类型检查保留。PosterBoard 的 render-target 纹理可以直接采样。
- **#7**：实现 `-[HakiosMetalDevice supportsSampleCount:]`，本任务交付的 payload 仅 `count == 1` 返回 YES。方法上一行标注了 MSAA render/resolve 完成后扩展到 4 的唯一位置。并行 MSAA 工作随后把共享树方法扩展为 1/2/4；没有覆盖其修改，本任务改用保留的 `build-20261110-105928` 用户态快照链接新编译的核心库，保持本任务要求的“只回答 1”。
- **#8**：render 的短 pushed-constant binding 使用已有 direct-constant 路径：复制真实逻辑长度内的字节，余下补零，不再以“映射后面还有多少页”拒绝或越界读取。覆盖 vertex/fragment 两阶段；完整绑定保持原来的 GPU indirect/copy 路径。短绑定在编码时取 CPU 快照，与已有 direct-constant 分支一致；不是 GPU 执行时重新读取。

**性能取舍**：没有给新页重复清零，也没有在每次内核池命中时增加 CPU memset。池归还时的清零仍消耗带宽，且当前在已有 GPU 锁内执行，可能增加 close/reap 时间；它只是把必要的清零放到回收路径，并没有消除成本。若观察到的 1.1 GiB/2 s 请求全部对应实际内核池周转，仍约有 0.55 GiB/s 的清零写流量（另有 cached clean），须由 profiling 实测。用户态 command/table pool 原有清零未改；IOSurface 已预清零的池命中不重复清。这里优先保证新分配不含旧内容；不把同一进程上轮内容当成可公开的新资源初值。

改动文件（均相对于 `work/umi-v23`）：

- `userland/src/HakiosMetalCA.m`
- `hakios-metal/src/render.cpp`
- `hakios-metal/tests/render_test.cpp`
- `devices/xiaomi-mi-10/overlays/xnu/files/iokit/Kernel/hakios.gpu.cpp`
- `devices/xiaomi-mi-10/overlays/xnu/files/iokit/Kernel/MiniIOSurface.cpp`（新增 umi overlay；上游模板未改）
- 本文档。

监督者实机检查：启动日志应出现以下完整行（说明清零策略所在内核已运行；仍需实际检查像素）：

```text
HakiosGPU: zeroed allocations: fresh pages and completed buffers before pool reuse
```

复现 PosterBoard 墙纸、照片、Maps/备忘录绘制及回桌面；下面原日志行应消失（PID 不固定，按其消息部分匹配）：

```text
texture 3 was made without shader-read usage; downsample_4_frag_lph samples it
-[HakiosMetalDevice supportsSampleCount:] is not implemented in this plug-in; it returns zeros
fixed_vert_lph_spc reads 64 bytes at 0 of constant buffer 3; 16 are bound and 16 mapped behind the offset
fixed_vert_lph_spc reads 64 bytes at 0 of constant buffer 3; 48 are bound and 48 mapped behind the offset
```

新资源未完成绘制时应为零内容，不应再露出其他旧图像；本次未实机确认 Apple logo 闪屏的最终表现。原有 `HakiosGPU: buffers:`、`HakiosGPU: ... submissions ...` 与 `fps:` 行用于对比清零后的时间/帧率；不要为本项增加 boot-args。

验证和产物：

- kernel、`MESA=1` iOS Metal 核心库、CA payload 构建成功，`prepare.py verify` 成功。
- 6 个本任务相关回归测试全通过；短常量测试检查 0/13/16/48 字节、0/16/64 字节读取起点、页尾与逻辑长度之后的毒化内存。完整常量的 indirect 路径及未对齐 GPU copy 路径也通过。日志：`out/xiaomi-mi-10/umi-metal-small/targeted-tests.log`。
- 全量 `make -C hakios-metal test`：284 tests，7 failed checks，分布于 5 个并行改动相关测试：`c_api_texture_over_an_iosurface_refuses_by_name`、`render_options_switch_each_group_and_each_alternative`、`a_stencil_attachment_is_the_s8_plane_and_its_test_the_draws_state`、`a_multisample_texture_is_its_samples_side_by_side`、`a_multisample_depth_is_tiled_at_its_samples_cpp`。未改动这些断言；完整输出在同目录 `metal-test.log`，不能记作全量通过。
- 对最终 bundle 执行要求的 `nm -u -m ... | grep "dynamically looked up" | grep -vc weak`，结果 **0**。最终 payload 来自 `flash/build-20261110-105928/userland/root.apfs`，用本任务固定保存的 `libhakios-metal.a` 重新链接；**请使用下列独立目录产物，不要拿共享 flash 输出替代**。
- `out/xiaomi-mi-10/umi-metal-small/` 内保留构建日志、用户态快照路径、改动文件清单和源码快照；kernel + payload + 4096 字节低于 63 MiB 限制。本次 kernel 是本任务构建时的快照，其他任务后续内核改动须由监督者合并重建。

产物均在 `work/umi-v23/out/xiaomi-mi-10/umi-metal-small/`，SHA256（另存 `SHA256SUMS`）：

```text
0efa73cb1fbf9744629456021824575de2e810f1cbab365ed46914964421a08c  kernel
e57f0cee6ecfaf2e68278cc528a9d38c36a6334efa1a11acc2948ec384ff54b4  root-flash-ui-ca.apfs
48a58b320957b3ea33da8b98ae54e61079948bf81f8db25fced7e62ad0659e5d  HakiosMetal
defcaf4238989294749cbff86820045e0c9c57df11a44b86903c03e5a4e000c5  libhakios-metal.a
```


## Fix #5 — umi-metal-pencil：保留 enqueue 顺序，修正 scheduled 时序（2026-11-10）

**确定的代码缺陷是命令队列违背 Metal 的 enqueue 顺序；它是本次实时笔迹闪烁的首要嫌疑，画面效果仍待主管上机验证。**
旧实现的 `-[HakiosMetalCommandBuffer enqueue]` 只打印警告。若 A 先 enqueue、B 后 enqueue，但 B 先 commit，GPU 会先运行 B，
因此合成当前笔迹/缓存底图的命令可能读取上一轮内容；松手后不再交错更新，最终图像可以正常。原日志只能证明 PencilKit 使用了 enqueue，
没有逐缓冲的 enqueue/commit 跟踪，不能声称已经从日志证实它发生了反序提交。

证据与其他候选的排查：

- `logs/test-1110-095629-umi-metal-all.union.log:6605`：MobileNotes 确实命中 `place reserved by enqueue is not kept`。
  代码还存在两个相关错误：延迟提交的 `waitUntilScheduled` 直接返回；核心在 commit 内同步报错或快速完成时，completed handlers 可以先于 scheduled handlers。
  依赖 scheduled 等待后直接 present（包括事务展示的调用方）也会受影响。
- 第 6596 行的 4512 字节 pitch 警告是真实的，但 Notes 的统计只记录 **2 次读入 / 27 ms、随后 1 次读入 / 7 ms，均为 0 次写回**。
  这不像持续写回实时画布的路径。shadow 在编码阶段刷新确实仍有共享副本风险，但现有日志没有证明它是此次闪烁原因，故本修复不扩大到重写 shadow。
- `MiniIOSurface.cpp` 已将默认行距及查询的对齐要求设为 64 字节；显式 `IOSurfaceBytesPerRow` 则保留调用方布局。
  4512 % 64 = 32；可以改分配策略的仅是我们拥有且允许变更布局的 surface，不能把包裹调用方内存的 surface 改 stride。
  本次无需改内核或增加一次额外拷贝。
- PencilKit 的 14 个 pipeline 在首个统计窗口累计编译约 30.5 ms，后续窗口为 0；现场编译能解释冷启动，不能单独解释持续局部闪烁。
  约 10 fps 的低更新率是另一项性能现象，此补丁不承诺提帧。
- `presentDrawable:` 原本已留住 drawable 并在完成回调中 present；本插件没有自建 CAMetalLayer drawable 池，未发现可由本日志证实的过早复用。
  颜色附件的 Load/Clear 已分别传递，清除在 pass 内仅消耗一次。Freeform 的应用记录存在，但合并日志没有保留下来 `HakiosMetal[Freeform ...]` 的驱动统计，不能用 Notes 的统计冒充它的证据。

修改只涉及本任务的以下文件（共享树同时含其他任务改动）：

- `userland/src/HakiosMetalCA.m`：每条队列保留 enqueue 位置；commit 隐式 enqueue；只把队首连续 ready 的缓冲按顺序送入同一串行提交队列。
  前项未 commit 时，后项 commit 正常返回但不执行；不为未 commit 的前项阻塞工作线程。事件等待也在同一提交序列中。
  保持缓冲及资源存活到完成；等待提交的 status 为 Committed；scheduled 等待不再提前返回；完成路径先交付 scheduled，再交付 completed。
  自定义 completion queue 仍按原接口异步派发 handler，修复保证先派发 scheduled。
- `devices/xiaomi-mi-10/tools/test-metal-pencil.py`：从实际 Objective-C 源文件抽取上述方法，在 macOS Foundation/dispatch 上使用可控提交桩测试；不访问 GPU/手机。
- 本文档：Fix #5。

主机回归包含 A enqueue、B 先 commit 的像素更新顺序（1→2）、隐式 enqueue、重复 enqueue、阻塞的 scheduled 等待、event 等待，
以及 300 次同步完成/同步错误/并发快速完成，均通过。`prepare.py verify` 通过。
构建与全量测试最终结果、交付 SHA256 见下方交付记录。

主管上机检查（保持已有 boot-args，不新增）：

1. 打开 Notes 手绘画布及 Freeform，持续画长线、回折、快速重叠线和橡皮，分别观察手指仍移动及松手瞬间，确认局部闪烁是否消失。
2. 每个进程第一次使用队列应出现以下日志正文（前缀为 `HakiosMetal[MobileNotes <pid>]:` 或 `HakiosMetal[Freeform <pid>]:`）：
   `queue-order: enqueue reservations honoured; scheduled handlers precede completion`
3. 如果应用确实反序 commit，首次应出现：
   `queue-order: commit held behind an earlier enqueue`
   此行出现说明实际触发了旧实现会越序的情形；没有此行不能宣称已证实闪烁由反序提交导致。
4. 旧正文 `a command buffer was enqueued; this plug-in runs command buffers in the order they are COMMITTED, so a place reserved by enqueue is not kept` 应消失。
5. 继续看 `fps:`、`fps: ... surface copies on the processor:`，以及 `refused:` / `gpu: no answer` / `THE SMMU REFUSED`。
   4512 字节 shadow 警告本身仍可出现；若闪烁仍在且出现频繁读入/写回，应继续针对 IOSurface 副本/共享 surface 的实际生产消费时序取证。


## Fix #1 — umi-metal-gpufault：IOSurface handle 交接、fence 回收和默认恢复（2026-11-10）

交付目录：`out/xiaomi-mi-10/umi-metal-gpufault/`。只做代码、主机测试和构建；未接触手机，A650 实际 reset/恢复仍由 supervisor 验证。

### 取证结论

- 原日志最后提交 15602，已完成 15601；IB 在 `0x35d30000`，fault 在 **另一个地址** `0x24980000`。FSR `0x80000402` 含 translation fault，FSYNR `0x3` 指向 level-3 translation/read fault。
- IOVA allocator 是 `[0x20000000, 0x100000000)` 的 first-fit、合并后重用空洞；`0x24980000` 在起点之后 **73.5 MiB**，16 KiB 对齐。普通 BO、IOSurface、shader、命令内存共用这个区间；它不是固定 ring (`0x01000000`)、SQE (`0x01100000`) 或 scratch (`0x01200000`)。**地址本身无法区分 BO/IOSurface，原日志没有分配/释放记录，不能追溯该次 fault 的具体 surface ID 或断言一定是 Home 释放。** 新增 512 条映射事件环形记录，仅 fault 时输出命中的最近记录和 allocator 是否 FREE。
- 已确认并用主机测试复现一个能造成此类故障的竞态：`command.cpp::surfaceBuffer()` 的 weak_ptr 在最后引用归零时已过期，但 deleter 尚未取得 `gSurfaceLock`；另一线程先拿锁、从内核取得同一 IOSurface handle，看到 weak_ptr 过期后创建新 Buffer。旧 deleter 随后关闭这个 handle，新 Buffer 的 IOVA 变成悬空地址。旧内核 submit 对不存在的 BO 直接 `continue`，仍把包含该地址的命令送到 GPU。
- 原内核并非完全没有 fence 生命周期管理：每个映射有 `b->fence`，IOSurface import 持有 `MiniIOSurfaceCopy`，各进程有自己的 IOVA，释放 App 的映射不会直接取消 backboardd 的另一份映射。原来的固定 64 项 `fLeft` 随 client 消失后丢失回收入口；溢出且等待失败时只保留 backing memory，却丢掉 surface 引用，alias 表可能留下悬空指针。
- UMI overlay 和生成内核中均没有 `hakios_gpu_recover`，FAULTS 恒为 0。原 `gpu_recover=1` 文案、FakeGpu 测试和 loader 白名单不代表 UMI 内核已有实现。

### 修改

1. 用户态按 `(Gpu, surface ID)` 在 import **之前**查找缓存；活对象直接复用。weak_ptr 已过期时等待旧 deleter 完成 close、移除 entry 后再 import。不会新建两个同时拥有同一 handle 的 Buffer。
2. 内核提交前逐一验证所有 BO 的 handle 和 presumed IOVA；任何失效/不匹配均拒绝整个提交。校验、ring submit 和 last-use fence 更新共用 `gGPULock`。
3. 延迟释放改为全局 retired 链表，持有 descriptor、IOSurface、IOVA、last-use fence；client 退出也不丢记录。后续 submit/close 在 fence 退休后才 unmap、释放 IOVA 和引用。内存不足时同步等 fence；恢复失败时保留完整引用，避免 DMA 写入重分配的内存。跨进程使用同一 IOSurface 的其他映射继续通过各自引用保活 backing memory。
4. 显示 alias 重映射前等待共享 `surface->drawn` 的最后 fence；等待 offscreen 时额外持有 surface 引用，防止解锁期间释放。
5. UMI 默认 `gpu_recover=1`，无需增加 boot-args；现有 `gpu_recover=0` 可禁用。等待 fence 时检测 SMMU fault，立即恢复；没有 SMMU fault 的普通 hang 仍按等待上限（Metal 为 5 秒）触发。暂停 SQE、确认 GBIF halt ACK 后，按 A650 `RBBM_SW_RESET_CMD` 的要求保持 reset 至少 100 us，再重建 ring 状态、清 fault/TLB、重跑 core 初始化和写内存自检。FAULTS 记录实际执行的 reset；自检成功后才把旧 fence 推到最后提交并重新接收提交。旧 ring 不重放，所有被丢弃的在途提交由用户态报告 error；后续提交可继续。总线不能停或自检失败则保留资源并停止接收提交，不假报恢复成功。

reset 寄存器/时序依据本地 Linux `mnt/mainline-src/linux/drivers/gpu/drm/msm/{adreno/a6xx_gpu.c,registers/adreno/a6xx.xml}`。这里使用保留 GMU/页表的 core reset，尚无本机运行验证；不能承诺只丢一个帧，已排队的提交会一起丢弃。

修改文件（相对 `work/umi-v23`）：

- `devices/xiaomi-mi-10/overlays/xnu/files/bsd/dev/hakios.adreno.c`
- `devices/xiaomi-mi-10/overlays/xnu/files/bsd/sys/hakios.input.h`
- `devices/xiaomi-mi-10/overlays/xnu/files/iokit/Kernel/hakios.gpu.cpp`
- `hakios-metal/src/command.cpp`
- `hakios-metal/tests/command_test.cpp`
- 本文（只追加本节）。没有修改 `userland/src/HakiosMetalCA.m`。

### 验证与交付

- kernel、CA payload 构建成功，产物立即复制至本任务目录；生成的三份 kernel 源文件与 overlay 相同。
- 新增 `surface_last_release_races_reimport`：4 线程共 40,000 次 import/release，检查活 Buffer 的 handle/IOVA。旧实现单测复现 **1380** 次失效；修复版为 **0**。旧实现对照放在任务目录单独编译，没有回退共享源码。
- AddressSanitizer 下 command 生命周期/恢复测试 **15 tests, 0 failed checks**，含丢弃旧提交后再提交成功的现有测试；这些是 FakeGpu 测试，不能替代硬件 reset 测试。
- 最后一次 `make -C hakios-metal test`：**287 tests, 0 failed checks**（`metal-test-final.log`）。最初并行任务的多平面/MSAA/模板测试有 7 项失败，旧实现对照也出现相同失败；最终共享树的这些测试已通过。
- `devices/xiaomi-mi-10/prepare.py verify` 通过。payload 内实际 bundle 的 `nm -u -m ... | grep "dynamically looked up" | grep -vc weak` 为 **0**。
- 构建日志、旧实现对照日志、ASan 日志、源码快照和本任务差异 `fix1.patch` 均在交付目录；`userland-ca-build-path` 记录实际 payload 构建快照。

| 产物（相对 `out/xiaomi-mi-10/umi-metal-gpufault/`） | SHA256 |
|---|---|
| `kernel` | `e68480b83eb6aa08bb138e5938616bf11e9cd93a6d7de490334ebaf211aaedd0` |
| `root-flash-ui-ca.apfs` | `5f27787d9405cbfd91f3e7b9497734e245e012b902cbcc0913127f8069248918` |
| `HakiosMetal`（核对用 bundle executable） | `ac17db3b3268ad4ea8626988720ac10a7f15c32930d2d7aec6a221eaba3c22e2` |

### supervisor 真机检查的日志

沿用 `metal_all=1`，不必额外塞 `gpu_recover=1`。重点复测 Home 进出、退出 App、widgets 连续更新超过原来的 10 分钟。正常启动必须有：

```text
adreno: gpu: automatic recovery enabled (gpu_recover=1)
```

若再次 fault，精确前缀/格式如下（数值随本轮变化；IOSurface 0 表示普通 BO/无 surface）：

```text
adreno: 0: THE SMMU REFUSED the 3D core: status ..., address ..., syndrome ...
adreno: gpu: fault IOVA ...: allocator FREE
adreno: gpu: mapping retired-unmap [...,+...) IOSurface ... last-use ...
adreno: gpu: recovery begin: last ..., completed ...
adreno: gpu: recovery 1 OK; submissions up to ... are given up as done; accepting new work
```

allocator 也可为 `allocated/reserved`，mapping 事件也可为 `surface-map` / `buffer-map` / `defer` / `unmap` / `cache-unmap`。它们用于判断被释放映射、仍有效映射上的越界、以及最后使用 fence；若最近 512 条没有命中，不能据此认定没释放。

用户态对应旧提交错误应为 `given up by a reset of the GPU ...` 或 `the GPU faulted or timed out; the kernel reset it (reset 1 of this run)`。**恢复成功必须同时看见后续 `HakiosMetal[backboardd ...]: fps:`、`fbstat:` 的 frames/swaps 继续增长并且 UI 能操作**，单独一行 `recovery ... OK` 不足以验证整条渲染链。

以下任一行要保留完整上下文回报：

```text
HakiosGPU: refused stale BO: handle ... presumed ... actual ...
adreno: gpu: recovery FAILED: bus did not halt; mappings retained
adreno: gpu: recovery FAILED: restart self-test; mappings retained
HakiosGPU: retaining busy mapping ... after allocation/recovery failure
```

`refused stale BO` 表示成功阻止一次错误提交，但仍需查其调用者；恢复 FAILED 表示本轮未恢复，不能报告“只丢帧”。

Fix #5 交付记录：

- Payload：`out/xiaomi-mi-10/umi-metal-pencil/root-flash-ui-ca.apfs`
  SHA256：`5f27787d9405cbfd91f3e7b9497734e245e012b902cbcc0913127f8069248918`
- Bundle 可执行文件：`out/xiaomi-mi-10/umi-metal-pencil/HakiosMetal`
  SHA256：`ac17db3b3268ad4ea8626988720ac10a7f15c32930d2d7aec6a221eaba3c22e2`
- 构建目录：`out/xiaomi-mi-10/flash/build-20261110-110521`；从该私有构建的 `userland/root.apfs` 立即复制，避免其他构建覆盖 flash 公共出口。
  编译用源码 `HakiosMetalCA.built.m`、源码清单 `source-sha256.json`、`SHA256SUMS`、本任务单独的 `pencil-only.patch` 及构建/测试日志均在交付目录。
  此 payload 是当时共享树的合并快照，包含其他任务同期的 Metal 改动，不是仅 #5 的独立 A/B 基线。
- CA payload 构建成功；`nm -u -m <build>/userland/root/lib/HakiosMetal.bundle/HakiosMetal | grep "dynamically looked up" | grep -vc weak` 结果 **0**。
  首次构建曾因共享库未重编而缺少另一个任务的 `_hakios_metal_texture_new_iosurface_plane`；重编 iOS `MESA=1` 静态库后重新构建并复查，交付文件不含该缺符号问题。
- 队列专项测试通过，已逐方法核对与交付源码快照相同；`devices/xiaomi-mi-10/prepare.py verify` 再次通过。
  全量测试前两轮捕获了其他任务尚在修改的双平面/MSAA 测试不一致，日志保留；最终 `make -C hakios-metal test` 为 **287 tests, 0 failed checks**（`metal-test-latest.log`）。
- 全程未执行手机命令、未增加 boot-args；未实机验证是否消除笔迹闪烁。

## Fix #4 — umi-metal-depth：sysmem 深度/模板与 4× MSAA（2026-11-10）

对应 `logs/test-1110-095629-umi-metal-all.union.log` 的 Maps `multisample Depth32Float_Stencil8` 创建失败，以及随后 Grid/Clear 的 `depth test ... with no depth attachment`。原来已有 D32、双面模板寄存器和颜色 resolve；本次补齐导致这条路径断开的格式、布局和独立模板处理。GMEM 默认仍为关闭，没有增加 boot-args。

- 支持 Depth16Unorm、Depth32Float、Depth32Float_Stencil8 和独立 Stencil8 附件；深度比较/写入、双面模板比较/三种操作及读写掩码沿用 A6xx 状态，支持 Clear、Load、DontCare 和 Store。sysmem 的 DontCare 不强制擦除，Store 会 flush；仅模板的 pass 也 flush depth CCU。
- D32S8 的 D32 和 S8 分别按样本数布局；修正多采样 stencil offset/pitch/size、D16 的 256 字节 tiled pitch 对齐，以及采样描述符的 PITCHALIGN。4× MSAA 的清除以恒定线性填充覆盖整个平面的有效行和 padding，避免按单采样 microtile 解释多采样布局。D32 与 S8 的 clear 独立，不会互相覆盖。
- 支持 1/2/4 样本颜色及深度/模板 render target；`supportsSampleCount:` 和 `supportsTextureSampleCount:` 同步返回 1/2/4。颜色在 `MTLStoreActionMultisampleResolve` / `StoreAndMultisampleResolve` 时，使用原有 2D resolve 路径，在全部绘制完成后执行；浮点/归一化颜色取样本平均，整数颜色取 sample 0。StoreAndResolve 保留源样本。校验 resolve 格式及目标 mip 尺寸。
- 独立模板附件可和不同的深度纹理绑定，也可单独使用；其 buffer 和 Texture 在命令完成前保活。补齐仅模板 pass 的窗口、默认 viewport、无颜色输出时的占位 MRT 格式和缓存 flush。
- 参考：`xnu-deps/build/mesa/mesa-25.0.7/src/freedreno/fdl/fd6_layout.c` 的 `fdl6_tile_alignment`，`fd6_view.c` 的样本数及 `samples_average`；`vulkan/tu_cmd_buffer.cc` 的 `tu6_emit_zs/tu6_emit_msaa`；`tu_clear_blit.cc` 的 Z16/Z32/S8 clear。没有改 compiler/ir3 或 pm4 协议。

范围：本次完成附件级 MSAA 和**颜色** resolve；未新增深度/模板 resolve、逐样本 fragment shading、shader sample-mask 输出、自定义采样位置或 alpha-to-coverage。模板附件目前为 level 0 / slice 0（含 D32S8 的独立 S8 平面）；多层模板不在本次范围。主机 fake GPU 校验命令、内存范围和线性 clear/resolve 数据，不模拟真实 tiled 光栅化，所以 Maps/快照是否恢复须由 supervisor 实机确认。

本任务改动文件（这些文件也有其他 agent 的同期修改，不能把整个 git diff 归为本任务）：

- `userland/src/HakiosMetalCA.m`：格式/附件接入、样本数查询、resolve 校验及日志。
- `hakios-metal/include/hakios-metal/texture.hpp`、`hakios-metal/src/texture.cpp`：D16/S8 格式和各平面布局。
- `hakios-metal/src/render.cpp`：附件绑定、clear、独立 stencil、flush、保活。
- `hakios-metal/src/c_api.cpp`：模板 Texture 保活及仅模板默认 viewport。
- `hakios-metal/src/fake_gpu.cpp`、`hakios-metal/tests/{texture,render}_test.cpp`：新格式/布局模型与回归覆盖。
- 本文档的 Fix #4。

验证：`make -C hakios-metal test` 为 **287 tests, 0 failed checks**；含 1125×2436 D32S8 的 1/2/4 样本平面范围、65×17 非对齐尺寸的四种深度/模板格式、全样本 clear、连续 Load 绘制后 resolve，以及仅模板 pass 的提交/flush。原有颜色 resolve 测试继续校验四个不同样本的实际均值。`devices/xiaomi-mi-10/prepare.py verify` 通过。最终 payload 另显式重编 iOS `MESA=1` 静态库后打包；CA build.sh 本身只链接已存在的库。

Supervisor 上机应在 Maps / `com.apple.MapKit.SnapshotService` 前缀下检查以下日志正文（每进程首次一次；记录的是编码成功，非 GPU 完成证明）：

```text
depth-msaa: encoded sysmem depth/stencil draw, samples=4, depth=1, stencil=1
depth-msaa: encoded colour resolve, samples=4
```

若 pass 没有绑定模板，第一行末尾为 `stencil=0`；并行 encoder 的 resolve 行为 `depth-msaa: encoded parallel colour resolve, samples=4`。首次 pass 若是单/双采样，计数相应为 1/2。

以下旧拒绝在 Maps 的目标绘制中应消失：

```text
refused: a texture: 1125x2436, pixel format 260
refused: a draw of Grid::grid_vertex+Grid::grid_fragment
refused: a draw of Clear::clear_vertex+Clear::clear_fragment
refused: the resolve of colour attachment
```

同时检查没有 `a depth test ... with no depth attachment`、`gpu: no answer`、`THE SMMU REFUSED`。画面需实际显示地图；SnapshotService 的 `flat picture: ... every sample RGBA8 (0, 0, 0, 0)` 不应再持续出现。全程只做代码、主机测试和构建，未触碰手机。

## Fix #6 — umi-metal-ycbcr2：App 侧 IOSurface 分平面纹理（2026-11-10）

已实现 `newTextureWithDescriptor:iosurface:plane:` 对 `420f` / `420v` 的原始平面视图：plane 0 的 R8Unorm、plane 1 的 RG8Unorm，以及按字节打包的更宽颜色格式。只做代码、主机测试和构建，未操作手机，未增加 boot-arg；视觉结果交 supervisor 验证。

**先核对请求方。** `logs/test-1110-095629-umi-metal-all.union.log` 中相关拒绝共 9 次，全部是 SpringBoard、`420f` 788×1706、plane 0、`MTLPixelFormatRGBA8Unorm`（70）、usage `0x2`（ShaderWrite）；两平面 pitch 都是 800，chroma 为 394×853。不是日志已经观察到 R8/RG8 请求。首例在 6038 行，其余为 7634、7766、7853、9898、17983、18272、19079、19867 行。

SpringBoard 在 3388 行编译了 VideoToolbox 的 `VTMTSComputeFunction`。对本机 20F66 arm64 shared cache 的 VideoToolbox 静态检查发现：

- `0x194f3ed48` 调用 `CVPixelBufferGetWidthOfPlane`，`0x194f3ed54` 将宽度除以内部纹理描述符的打包因子；高度来自相应 plane。
- `0x194f3edc0..0x194f3ede4` 比较 Metal texel 字节数与 `IOSurfaceGetBytesPerElementOfPlane × 打包因子`，随后 `0x194f3ee10` 调用 `CVMetalTextureCacheCreateTextureFromImage`。
- `420f/420v` 分支 `0x194f4979c..0x194f497b4 → 0x194f4a958` 包含格式 70 的打包写入配置，以及格式 10/30 的平面配置。因此 RGBA8 是一次写入四个 Y 字节或两组 CbCr 的原始字节视图，不应执行 YCbCr→RGB 转换。788 字节的 Y 行对应 197 个 RGBA8 texel。

原日志没有 texture descriptor 宽高或调用栈；上述调用链由二进制和已运行的 VTMTS shader 对应，具体 descriptor 宽高由新增日志在下一轮确认。可复核地址与反汇编片段见 `out/xiaomi-mi-10/umi-metal-ycbcr2/rgba-plane-evidence.txt`。

**实现。** Objective-C 入口用 IOSurface 的 plane base、width、height、bytesPerRow、bytesPerElement 获取布局，offset 相对于整块 surface base；验证纹理的实际字节宽度、plane 高度及 allocation 范围。对齐的布局通过新增 C API `hakios_metal_texture_new_iosurface_plane` 进入已有 `Texture::wrapSurface` / `surfaceBuffer`，两个 plane 和其他视图共享同一个 IOSurface buffer。`SurfacePlane` 增加宽高；每个 plane 复用已有线性导入与较宽 texel 检查，不猜测 chroma offset，不复制 YCbCr 转换器。

800-byte pitch 不能直接用于 Adreno 的 64-byte pitch 单位。因此未对齐的 plane 复用既有 shadow：`hakiosShadow:plane:`、refresh、完成后的 writeback 均选择该 plane 的基址/行距，只处理纹理高度内的行。保留原来的 seed 刷新和差分写回，避免写坏另一个 plane、padding 或同 surface 的其他绘制。私有 500/540 格式仍走原有完整图片转换路径；本任务没有修改其转换算法。构建包含并行任务当时已有的 GPU conversion v8。

本任务改动文件（相对 `work/umi-v23`）：

- `userland/src/HakiosMetalCA.m`
- `hakios-metal/include/hakios-metal/{texture.hpp,hakios_metal.h}`
- `hakios-metal/src/{texture.cpp,c_api.cpp}`
- `hakios-metal/tests/texture_test.cpp`
- `devices/xiaomi-mi-10/tools/test-metal-iosurface-planes.py`
- 本文的 Fix #6 section。

**验证。** `make -C hakios-metal test`：285 tests, 0 failed checks；新增 core 测试涵盖 420f/420v、奇数宽高、不同 pitch、非零 offset、R8/RG8 与 RGBA8 共用内存、两平面隔离、错误 plane/跨度/纹理宽高和整数边界。`python3 devices/xiaomi-mi-10/tools/test-metal-iosurface-planes.py` 抽取实际 CA shadow 方法，以 mock IOSurface 在 ASan/UBSan 下验证原始与打包纹理的刷新、padding 保留及独立写回，PASS。`make -C hakios-metal ios MESA=1 LLVM_AS=/opt/homebrew/opt/llvm@16/bin/llvm-as`、规定的 CA payload build、改动文件 `git diff --check`、最终 `devices/xiaomi-mi-10/prepare.py verify` 均通过。中间曾观察到并行 MSAA 测试及 battery 模板编辑的临时失败，最终重检已通过。bundle 的 `nm -u -m ... | grep "dynamically looked up" | grep -vc weak` 为 **0**。

**交付。** 构建快照：`out/xiaomi-mi-10/flash/build-20261110-111658`。完成后立即复制至独立目录；后续请使用这里的文件，避免共享 flash 输出被别的构建覆盖。此 payload 包含共享树在构建时的并行修改，不是仅 Fix #6 的独立基线。

| 路径（相对 `work/umi-v23`） | SHA256 |
|---|---|
| `out/xiaomi-mi-10/umi-metal-ycbcr2/root-flash-ui-ca.apfs` | `93c0f0d458e3d8941e76604093b83fb2f6c452d8dc0ac8b8d4ad7601aa2381f0` |
| `out/xiaomi-mi-10/umi-metal-ycbcr2/HakiosMetal` | `63e6049a9961c731a261385099eccf98d8fe742d0175a315d8602ae83f01ef2d` |

同目录有 `SHA256SUMS`、`build-path.txt`、`source-sha256.json`、`payload-build.log`、`ios-build.log`、`host-tests.log`、`plane-shadow-tests.log`、`prepare-verify.log` 和 `nm-undefined.txt`。

**手机日志检查（supervisor）。** 新日志每进程最多前 32 个 raw-plane import，完整格式为：

```text
iosurface-plane: IOSurface N '420f' plane 0, format 70, texture 197x1706, plane 788x1706, offset O, pitch 800, usage 0x2: raw plane shadow
```

其中 N、O 取真实 IOSurface ID/offset，`texture 197x1706` 是打包因子为 4 时的预期值，应核对实测。标准分平面采样应看到：

```text
iosurface-plane: IOSurface N '420v' plane 0, format 10, texture WxH, plane WxH, offset O, pitch P, usage 0x1: shared plane
iosurface-plane: IOSurface N '420v' plane 1, format 30, texture CWxCH, plane CWxCH, offset O, pitch P, usage 0x1: shared plane
```

`420f` 同样支持；未对齐的 pitch/offset 会显示 `raw plane shadow`。写入方的 plane 1 也可能使用打包格式 70，应按日志中的 texture/plane 宽度核对字节数。旧 `has 2 planes: a single-plane surface is what is served` 和 `plane 1 ... a single-plane surface (plane 0)` 拒绝应在这些 420 请求上消失。backboardd 的转换路径另查当前构建的 `ycbcr: GPU conversion v8: aligned plane copies, format 500`；原始平面成功导入本身不触发这个转换日志。最终需检查动态壁纸/视频/控制中心画面及 GPU fault 日志，主机测试不能证明硬件采样和视觉正确。

Fix #4 最终交付（显式重编静态库后的 payload）：

- Payload：`out/xiaomi-mi-10/umi-metal-depth/root-flash-ui-ca.apfs`
  SHA256：`b7f7d837a64c849afcb2822011615c5e0830237edb18b4b30f60239ed4926316`
- Bundle：`out/xiaomi-mi-10/umi-metal-depth/HakiosMetal`
  SHA256：`bd7ddb0ac9b30a2c90bfaae03a06496968c0a4a5cd7ec1da8b2fffce652cdbd8`
- 构建快照：`out/xiaomi-mi-10/flash/build-20261110-112406`。从该快照的 `userland/root.apfs` 立即复制交付，避免公共出口被其他构建覆盖。
- 最终 `nm -u -m <build>/userland/root/lib/HakiosMetal.bundle/HakiosMetal | grep "dynamically looked up" | grep -vc weak` 为 **0**。
- 交付目录含 `SHA256SUMS`、`build-path.txt`、`HakiosMetalCA.built.m`、`source-sha256.json`、`nm-check.txt` 及主机测试、iOS 静态库和 CA 构建日志。该 payload 是构建时共享树的合并版本，包含其他 agent 的同期修改。
