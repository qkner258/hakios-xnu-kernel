# Safari History — 2026-11-10

## 根因

`com.apple.Safari.History`（launchd `user/501`，程序在 App cryptex `/System/Cryptexes/App/usr/libexec/`）启动 0.1–0.2 s 内 SIGABRT：

```text
com.apple.Safari.History[228:3492] *** Terminating app due to uncaught exception 'NSInvalidArgumentException', reason: '*** -[NSURL initFileURLWithPath:isDirectory:]: nil string parameter'
```

SafariCore 初始化容器 URL 时先调 `sandbox_container_path_for_pid(getpid())`：返回 0 就把缓冲区当容器路径；返回非 0 才走自带的回退
`LSApplicationRecord(com.apple.mobilesafari).dataContainerURL`。我们的 minisandbox（`kern_minimac.c` 请求 4）对没有 `CFFIXED_USER_HOME`
的进程返回 **0 且不写缓冲区**，SafariCore 拿空串构造 NSURL 后崩溃。真机上无容器进程得到的是错误，不是成功。

崩溃早于 v8：metal-all（01:57）、v7（02:48，连续崩溃 8 次）、v8（04:26）都一样。服务一直起不来，所以没有 History.db，重启后历史自然消失。

## 修复

`overlays/xnu/files/bsd/kern/kern_minimac.c`（覆盖 `xnu/files` 同名文件）：环境里找不到 `CFFIXED_USER_HOME=` 时返回 `ENOENT`。
SafariCore 于是走 Apple 自己的回退，用 Safari 已注册的数据容器
`/private/var/mobile/Containers/Data/Application/<UUID>/Library/Safari/History.db`，位于持久 HFS `/private/var`。

删除了之前的注入方案（`libhakios.safari.dylib`、INJECT 行、`tools/test-safari-history.c`）。它在 v8 已加载（mediaserverd 走同样的
xpcproxy 路径，注入正常），但 `__interpose` 只能改写经过 GOT 绑定的调用；SafariCore 和 libsystem_sandbox / libsqlite3 都在 dyld shared cache 里，
它们之间的调用不经过我们的替换，所以一行日志都没有。

## 验收

- 不应再出现：`com.apple.Safari.History[...] *** Terminating app ... nil string parameter`、`(user/501/com.apple.Safari.History [N]) <Notice>: exited due to SIGABRT`、`service has crashed N times in a row`。
- 应出现：`(user/501/com.apple.Safari.History [N]) <Notice>: service state: running`，之后没有紧接着的 SIGABRT（空闲退出 `exited with exit code: 0` 或 `exited due to SIGKILL | sent by launchd` 正常）。
- 访问一个页面，重启，打开历史列表，页面仍在。
