# UMI 墙纸预览：PosterBoard 的 Metal 设备可见性

2026-11-10，iOS 16.5 / 20F66。仅 host 调研、修改与构建，未访问手机。

## 结论

已定位并修复一个确定的致命阻塞：`HakiosMetalCA.m` 默认向 PosterBoard 返回 nil Metal device，
但 `-[PBFHomeScreenPreviewImageBuilder _imageForView:]` 必须用 Metal command queue 创建
CARenderer 的选项字典。nil queue 导致 `NSInvalidArgumentException`，PosterBoard SIGABRT，
launchd 随后清理它托管的全部 poster 扩展，Settings 得不到完成的预览。

这属于 **CA 预览生产端的 Metal 设备准入错误**。现有证据并未显示 IOSurface 跨进程导入或
remote layer 合成本身是此次崩溃原因。修复只放行 `PosterBoard` 进程，继续使用已有的
CARenderer、IOSurface、Metal queue 和 shader 实现，没有追加 fallback、shim 或伪造 persona。
视觉效果仍须 supervisor 上机验证，host build 通过不等于预览已在手机验证成功。

## 日志来源与链条

主日志：`/Users/xm666/X/umi-ios/logs/test-1110-091804-umi-v6.kernel.log`。
同时使用 `out/xiaomi-mi-10/runtime/logs/091830-run.log.live.*` 的中间快照：日志环会滚动，
结束时的 kernel/live 文件不能代替早期快照中的所有 userspace 日志。尤其第一次异常在 `.169`，
第二、第三次异常完整 throw stack 在 `.310`、`.311`。已将相关记录按行去重，保留原快照名和
行号到 `out/xiaomi-mi-10/wallpaper-debug/v6-evidence.txt`；它是证据索引，不是完整无损合并日志。

下表为北京时间（syslog 原时间 UTC + 8）；相邻事件不意味着每一步都有独立的函数调用日志。

| 时间 | 可确认的事件 |
| --- | --- |
| 09:18:49 | launchd 注册 `com.apple.PosterBoard`；usermanagerd 已被启动为 PID 20。 |
| 09:19:08.777 | PosterBoard PID 158 `Successfully spawned`。此时尚未启动 Preferences，说明初始启动也服务于系统墙纸。 |
| 09:19:10.023–.452 | extensionkitservice PID 160 和下列八个 poster 扩展依次成功启动。 |
| 09:19:10.550 | PosterBoard 查询 `com.apple.proactive.FaceSuggestions.xpc` 失败，但进程随后存活数分钟。 |
| 09:19:35.463 | Preferences PID 184 经 `UIKitApplication:com.apple.Preferences[…][rb-legacy]` 启动。 |
| 09:24:41.162 | PosterBoard 158 被拒绝 Metal device 后，在预览 builder 中抛出 nil 字典对象异常，SIGABRT；launchd 立刻重新启动 PID 227。 |
| 09:24:46.580–.773 | PID 227 的 extensionkitservice 与八个扩展均再次成功启动。 |
| 09:24:57–09:25:05 附近 | backboardd 的 scene/touch 记录含 Preferences context `10533D6` 和 PosterBoard context `254FCBBC`，后者收到 touch IDs `22-23`。宿主 remote context 存在，不代表预览图已渲染。 |
| 09:29:50.929 | Preferences scene 再次 foreground，context `95DD7E9A` / `45E3F9E2`。 |
| 09:29:53.766 | PosterBoard 227 再次因同一 nil queue 字典异常 SIGABRT；kernel 第 14650 行也记 `userabort` 和 cache slide。 |
| 09:29:53.783 | 重启 PosterBoard PID 270。 |
| 09:29:56.753–09:29:57.169 | extensionkitservice 与八个扩展第三次成功启动。 |
| 09:29:57.856 | PosterBoard 270 第三次相同异常；kernel 第 14743 行记 `userabort`。 |
| 09:29:57.885–09:29:58.011 | launchd 清理宿主的 process-scoped services：扩展 SIGTERM，extensionkitservice SIGKILL，明确注明 `after host exited`。这是宿主退出的后果。 |
| 09:30:06–09:30:07 | PosterBoard 285 和各扩展再次被启动；单凭重启成功不能认定预览恢复。 |

扩展 PID 对照，均有 `Successfully spawned` / `service state: running`：

| 服务 / 可执行程序 | 宿主 158 | 宿主 227 | 宿主 270 |
| --- | --- | --- | --- |
| extensionkitservice | 160 | 228 | 271 |
| GradientPosterExtension | 161 | 229 | 272 |
| CollectionsPoster | 162 | 230 | 273 |
| ExtragalacticPoster | 163 | 231 | 274 |
| WeatherPoster | 164 | 232 | 275 |
| EmojiPosterExtension | 165 | 233 | 276 |
| PridePosterExtension | 166 | 234 | 277 |
| UnityPosterExtension | 167 | 235 | 279 |
| PhotosPosterProvider | 168 | 236 | 280 |

另有 SpringBoard PID 8 所属的 CollectionsPoster PID 172，不能混作 PosterBoard 的子服务。

Settings 的墙纸入口在该系统镜像的
`/System/Library/PrivateFrameworks/Settings/WallpaperSettings.framework`，由 Preferences 加载，
通过 PosterBoardServices / PosterBoard 的预览与场景接口显示墙纸。日志没有记录进入 bundle 的
准确函数/时间，所以不将 Preferences 启动时间冒充用户点击“墙纸”的时间。用户操作报告、
Preferences foreground、PosterBoard 的 preview builder throw stack 共同界定了此次失败。

## 为什么是 nil Metal queue

三次异常均为：

```text
*** -[__NSPlaceholderDictionary initWithObjects:forKeys:count:]: attempt to insert nil object from objects[1]
```

前面都出现：

```text
HakiosMetal[PosterBoard ...]: PosterBoard is not shown the device: the window server alone is (metal_all=1: every process)
```

完整 throw stack 的第一个业务帧为 `0x1c4943aa8`。kernel `userabort` 明确给出
shared-cache slide `0x3f34000`，因此原始地址是 `0x1c0a0faa8`。
使用本地 20F66 dyld cache 和已提取的 PosterBoard.framework 反汇编可还原：

```text
PBFHomeScreenPreviewImageBuilder::_imageForView: 入口 0x1c0a0f8fc
0x1c0a0f9a4  MTLCreateSystemDefaultDevice() -> x19
0x1c0a0fa0c  [x19 newTextureWithDescriptor:... iosurface:... plane:0] -> x23
0x1c0a0fa48  [x19 newCommandQueue] -> x24
0x1c0a0fa70  keys[0..1] = { kCARendererColorSpace, kCARendererMetalCommandQueue }
0x1c0a0fa74  objects[0..1] = { colorSpace(x25), commandQueue(x24) }
0x1c0a0faa4  NSDictionary dictionaryWithObjects:forKeys:count:  // count 3，在此抛出
0x1c0a0fac0  [CARenderer rendererWithMTLTexture:x23 options:...] // 抛出时尚未到达
```

所以 nil 的不是用户照片或 persona 字典，而是第 2 个对象 commandQueue。Metal 设备入口返回 nil，
随后对 nil 发送 `newCommandQueue` 自然也得到 nil。程序在 CARenderer 建立前已经中断，不能把
这次失败解释为“CA 收到完整预览图后 Metal 没画出来”。backboardd 已有 PosterBoard context，
但这些日志没有提供某个预览 IOSurface 的 producer→consumer 对应关系或有效像素证明。

本地可复核资料（仅 host 文件读操作）：

- `out/xiaomi-mi-10/wallpaper-debug/symbolize-preview.py` 与 `preview-symbols.txt`：解析 cache selector、
  Metal 优化 stub，确认 `_imageForView:`、`newCommandQueue`、`rendererWithMTLTexture:options:`。
- 同目录 `posterboard-disasm.txt`：原始反汇编，包含字典 keys/objects 的寄存器与栈槽。
- 本地二进制来自 `tools/tmp/dsc-20F66`，不打包进 payload 或提交仓库。

## 其他候选原因的判断

- **persona**：本树 XNU `bsd/sys/persona.h` 定义 type 6 为 `PERSONA_SYSTEM_PROXY`，不是
  “wallpaper persona”。报错的 3 为 ESRCH，意为该类型未找到。相同错误发生在成功的 spawn 前，
  以上 PID 均随后 running；不能把这条警告当成拒绝启动。usermanagerd 已存在，container manager
  也已有 `personasAreSupported` 的 bring-up hook。本修复不伪造 usermanagerd、不分配假 persona。
- **缺失服务**：FaceSuggestions 的真实 owner 为 `com.apple.duetexpertd`，其 plist 发布该 Mach
  service；UMI 的 mkjobs DENY 明确排除了 duetexpertd（既有崩溃重试问题）。这会影响人脸照片建议，
  但不是上面已经定位的 CARenderer queue 异常；不为修复预览而启用这个 daemon。
- **ExtensionKit / sandbox / RunningBoard**：extensionkitservice 是宿主的 XPC service，不需要另造
  一个 system launchd plist。没有发现对应这些 PID 的 sandbox 拒绝或 RunningBoard assertion
  失败能解释该崩溃。日志未开启完整 assertion 跟踪，因此不声称所有 assertion 均已验证。
  MediaConversionService Image/VideoConversion 的 `Invalid XPCService dictionary` 也存在，
  不能由此推导所有 poster 扩展均未启动；它们不是此次 throw stack 的路径。
- **持久配置/图片**：无法仅由现有日志审计 `/var` 上所有 poster 配置及快照内容；历史 SpringBoard
  注释也不是此次 data volume 的实时清单。但这次确切的 nil 是 commandQueue，与某个图片路径
  缺失无关。先修复确定的生产端崩溃，首轮保留原持久数据进行复现。
- **更早日志**：v5 `test-1110-084726-umi-v5.kernel.log:6226` 同样记录 PosterBoard 被隐藏 Metal
  device；该文件没有保留这次完整 throw stack，因此不把它当作相同用户操作已成功或失败的证据。

## 改动与 host 验证

唯一代码改动在 `userland/src/HakiosMetalCA.m` 的 `initWithAcceleratorPort:` 进程判断，增加
精确的 `strcmp(name, "PosterBoard") == 0` 分支。不扩大为所有 poster 扩展或所有进程；
`metal_rs=1` 对 SBRendererService 的既有行为保留。没有修改其他人的 YCbCr、帧率、idle、USB、modem 工作。

**无新增 boot-arg**，不用改 loader allowed[]，607 字符预算增量为 0；无需重建 kernel 或 loader。
不需要手工修改 `/var`，也没有运行任何持久数据修改命令。

按任务指定命令构建，使用现有 build-lock：

```sh
source ../../env.sh
env FLASH_CA=1 METAL_PLUGIN=ca EXTRA_SHARE=/Users/xm666/X/umi-ios/work/umi-v23/out/xiaomi-mi-10/ca-share bash devices/xiaomi-mi-10/flash/build.sh userland
```

- 构建快照：`out/xiaomi-mi-10/flash/build-20261110-093724`。
- `nm -u -m <build>/userland/root/lib/HakiosMetal.bundle/HakiosMetal` 中
  非 weak `dynamically looked up` 符号：**0**。
- `make -C hakios-metal test`：**277 tests, 0 failed checks**（既有 host suite；不是设备像素测试）。
- 构建日志 `out/xiaomi-mi-10/wallpaper-debug/build.log`；undefined 符号完整清单、host test 输出及
  source/bundle/payload hashes 分别在 `bundle-undefined.txt`、`host-tests.log`、`validation.json`。
- 仅本任务的补丁保存为 `out/xiaomi-mi-10/wallpaper-debug/wallpaper-only.patch`，便于共享工作树审阅。

## Payload 与 supervisor 验证

保留一份有独立名称的 payload，避免其他并行 build 覆盖通常的 `root-flash-ui-ca.apfs`：

```text
/Users/xm666/X/umi-ios/work/umi-v23/out/xiaomi-mi-10/wallpaper-debug/root-flash-ui-ca-wallpaper.apfs
size:   20955136 bytes
SHA256: c01339d2a31e4da38aad0014dc570cfa8676650bbe25b1f3967b6b9e85bc4923
```

共享树的其他修改按照构建快照一并打包；此 hash 对应该固定快照。supervisor 如需包含后续其他
agent 的修改，重新运行上述 host build 并重新记录 hash。

上机由 supervisor 用既有测试流程，选择这个 payload，沿用 v6 的 kernel、loader、flashboot
和 boot-args，打开 Settings → 墙纸，重复进入预览/返回，并检查锁屏、主屏和不同类别的预览：

1. 预览实际请求设备时出现一次：
   `HakiosMetal[PosterBoard PID]: PosterBoard is shown the device: CARenderer wallpaper previews require a Metal command queue`。
2. 不再出现 PosterBoard `is not shown the device`、相同 nil 字典对象异常及 `userabort`/SIGABRT；
   不再随打开预览发生整个扩展组的 host-exited teardown。单独的生命周期退出不算失败。
3. PosterBoard 进入既有 Metal queue/texture/pipeline 路径，预览显示有效墙纸像素。检查是否出现新的
   `newTexture...` 拒绝、pipeline/缺失 selector 报错或 GPU timeout。新增这条放行日志只证明执行到了
   设备准入，不单独证明 queue 创建或 GPU 渲染成功。
4. persona type 6 和 FaceSuggestions 警告可能仍存在；不要将其消失作为此次修复的验收条件。
5. 若宿主稳定但某类图仍空白，保留对应扩展 PID、CA/IOSurface/Metal 日志再定位。首轮不删除、
   重置或覆盖持久快照/配置；本交付没有需要 supervisor 修改 `/var` 的步骤。

回退仅需恢复此前 payload，或撤销 `wallpaper-only.patch` 后重建；没有持久格式迁移。
