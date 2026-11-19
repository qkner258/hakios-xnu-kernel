## 2026-11-11：内核 IPv4 数据通路（待实机验证）

v17 的 `UMI MODEM` 日志已有 DMS online 成功、NAS `reg=1 cs=1 ps=1 rat=8`，
但尚未调用 WDA/WDS，也没有 IP_HW0 网络接口。本次补齐这条通路，保留原有 IPCR/EFS 服务。
既有 `umi_modem=1` 路径在 PS 注册及服务发现完成后自动拨号一次，无新增 boot-arg。

- 参考 Linux `pci_generic.c` 的 umi 表：IP_HW0 100/101，ER4/5，硬件突发门铃；处理 OOB/DB_MODE。
- 参考 ModemManager `mm-port-qmi.c` / `mm-bearer-qmi.c` 的 mhi_net/rmnet 路径：
  WDA raw-IP + QMAP、PCIe endpoint type **3** / interface **4**，WDS mux **1**。
  本实现固定使用最简 QMAP v1（协议值 5，无 checksum 扩展），下行 32 包/32768 字节，
  必须校验 WDA 返回格式；Linux 成功记录证明 QMAP 路径，未记录最终协商版本。
  修正原未接入的 WDS helper 的 embedded/1，使用独立且持续存在的 WDS QRTR 客户端端口。
- WDS bind → IPv4 family → `cmnet` start → runtime settings；建立 raw IPv4 `pdp_ip0`，
  配置地址、掩码、对端网关和 MTU。TX 有界队列交给原 service 线程；RX 拆 QMAP 聚合后交给 XNU IPv4。
  每 30 秒记录 TX 完成数、RX 上送数和丢包数。拨号失败只记录错误，继续控制面服务；无自动重拨。
- 默认路由和系统 DNS 留给 supervisor，避免改变 Wi-Fi 路由。CommCenter、校准存储和分区写入路径无改动。

主机验证：`python3 devices/xiaomi-mi-10/tools/test-modem.py`（新增状态化假基带交互、
格式/拨号拒绝、聚合/填充/越界、门铃/完成/回绕/断线测试）；`prepare.py verify`；
source `env.sh` 后通过 `build-lock.sh bash devices/xiaomi-mi-10/build-kernel.sh` 编译。
这不代表已在手机 ping 成功。

下次日志：

```sh
grep -aE 'UMI MODEM: (channel open id=10[01]|WDA |WDS |data up |data packets |data failed |MHI bad transfer)' <本次日志>
```

应依次看到 WDA/bind/family result=0、`WDS start network apn=cmnet result=0 handle=...`、
`data up pdp_ip0 ipv4=... gw=... dns=... mtu=...`，测 ping 时 tx/rx 均增长。

supervisor 通过 SSH 进入手机后，先用单个外网地址测试（可保留 Wi-Fi SSH）：

```sh
ifconfig pdp_ip0
CELL_IP='<data up 日志中的 IPv4，不含 /前缀>'
route -n add -host 1.1.1.1 -interface pdp_ip0
route -n get 1.1.1.1                 # interface 必须为 pdp_ip0
ping -S "$CELL_IP" -c 5 1.1.1.1
netstat -ibn -I pdp_ip0
route -n delete -host 1.1.1.1
```

完整默认路由测试：使用 USB SSH，关闭 Wi-Fi，确认 `netstat -rn -f inet` 已无默认路由后：

```sh
CELL_GW='<data up 日志中的 gw>'
CELL_DNS='<data up 日志中的首个 dns>'
route -n add default "$CELL_GW"
route -n get 1.1.1.1
ping -c 5 1.1.1.1
nslookup example.com "$CELL_DNS"    # 若系统有 nslookup，可独立验证运营商 DNS
route -n delete default "$CELL_GW" # 恢复 Wi-Fi 前删除本次测试路由
```

## From the mainline port

Research/build date: 2026-11-09. This section supplements the earlier MODEM-umi.md and supersedes its speculative integrated-modem/PCIe1 assumptions. The modem is an **external SDX55 on PCIe2**. Reference trees and partition backups were read only. No phone commands, downloads, firmware writes or subscriber-identifier logging were performed.

### Verified recipe and evidence

The authoritative reference is `/Users/xm666/X/mainline/umi-linux/work/modem-data/README.md`, verified on 2026-10-22 with Debian 13, ModemManager 1.24.0/libqmi 1.36.0 and China Mobile. **Registration, signal reporting, IPv4 Internet and SMS send/receive worked.** APN is `cmnet`. Logs `57-tftp-late-boot.txt`, `58-cellular-internet.txt`, `59-cellular-ping.txt`, `60-sms-self-test.txt`, `63-reboot-auto-internet.txt` and the final 64 check establish home/attached state, interface-bound HTTPS/ping, a self-SMS round trip, and automatic connection after reboot. No voice/IMS, IPv6, mobility, sustained throughput, suspend/resume or Wi-Fi handover claim follows from these tests. MM reported NR access but NR-only operation was not measured. Do not print private SMS, IMSI, IMEI or phone numbers when inspecting these files.

The initial `/Users/xm666/X/umi-ios/mnt/mainline-src` snapshot is incomplete in userspace, but its Linux branch `umi`, HEAD `c03c1a14507dfaf8b856c2608f9ac216021fc222`, Linux 7.3, contains essential uncommitted PCIe, MHI and WWAN changes. The final modem-data kernel additionally adds mission EFS channels. Trust the working PCIe2 DTS and the decompiled `out/umi-debian13-final/{boot.dtb,sm8250-xiaomi-umi.dtb}` over `research/03-modem.md`'s stale PCIe1 description. Final `config`/`cmdline.txt` and DTS were inspected; a local, unshipped decompilation is in `out/modem/linux-final.dts`.

The later reference also contains `work/dts/refs/vendor/drivers/esoc/esoc-mdm-{pon,4x}.c` and vendor `kona.dtsi`. SDX55's reset operation is **`qpnp_pon_modem_pwr_off(PON_POWER_OFF_WARM_RESET)` over SPMI**, not the GPIO soft-reset used by older modems. ESOC's first-power path requests this reset, waits 150 ms, raises AP_STATUS and requests userspace images when no PBLRDY GPIO exists. Its SDX55 ops have no GPIO PON setup or cold-reset operation; power-down also uses warm reset. The available vendor fragment does not contain the underlying QPNP register implementation. Mainline does not execute that ESOC path: it relies on inherited power and steady GPIO hogs. Therefore no PMIC PON/GPIO number or register write is invented in HakiOS; a fresh PBL boot is a prerequisite, not a software cold-reset capability.

### Minimal board sequence established by the sources

1. Use the **external SDX55 on PCIe2**, not MPSS/PAS4. Mainline disables PCIe1. AP2MDM_STATUS is TLMM **56**, output high; AP2MDM_ERRFATAL **57**, output low. MDM2AP_STATUS **3** and MDM2AP_ERRFATAL **1** are inputs. AP outputs use 16 mA/no pull, modem inputs 8 mA/no pull. All these pins are in the **SOUTH tile, physical `0x0f500000 + pin*0x1000`**, not Wi-Fi's NORTH tile.
2. Hold **PERST# TLMM85** low (GPIO, 2 mA/pull-down). **CLKREQ#86** uses `pci_e2`, mux1, 2 mA/pull-up; **WAKE#87** is a GPIO input, 2 mA/pull-up. These are also SOUTH-tile pins.
3. Vote **PM8150 L5A / RPMh `ldoa5` = 880 mV** and **L9A / `ldoa9` = 1200 mV**. PHY `vdda-phy` uses L5A, PHY `vdda-pll` and controller `vdda` use L9A. The umi modem DTS deliberately has **no vddpe-3v3 supply**: Linux uses a dummy regulator. Do not infer L11C from lmi. There is no PMIC GPIO PON/enable in this mainline path: it depends on **firmware-established modem power**, and the GPIO hogs represent steady running levels only. This does not implement a cold PMIC reset or crash recovery.
4. PCIe2 GDSC is GCC `+0x6004`; check power-on before any PARF/DBI/PHY access. PCIe2 controller reset is `+0x6000`, PHY reset `+0x1f01c`. AUX RCG `+0x6038` selects 19.2 MHz XO. Shared PHY REFGEN RCG `+0x6f014` selects **GPLL0 / 6 = 100 MHz**, config `0x10b`; PCIe2 REFGEN gate is `+0x6f034`. MDM clock-reference gate is `+0x8c00c`, PHY AUX `+0x6f004`, AGGRE PCIe TBU `+0x9000c`, DDRSS PCIe SF TBU `+0x8d058`. GCC base is `0x00100000`.
5. Vote GCC `+0x52010` bits **10=SLV_Q2A, 11=SLV_AXI, 12=MSTR_AXI, 13=CFG_AHB, 14=AUX, 15=PIPE**. Halt/status registers are respectively `+0x6010/14/1c/24/28/2c`. Keep PIPE until the PHY setup reaches its enable step. Assert/deassert owned resets with 1 ms delays. Check non-PIPE clocks before touching their register domains.
6. Controller resources: PARF `0x01c10000`, DBI `0x64000000`, ELBI `0x64000f20`, iATU `0x64001000`, CFG0 `0x64100000`, host MHI auxiliary region `0x01c13000`. PCI memory starts at `0x64300000`; this implementation assigns BAR0 there after sizing it. These are different from the QCA6390 windows.
7. PHY is **`qcom,sm8250-qmp-modem-pcie-phy`**, using `sm8250_qmp_gen3x2_pciephy_cfg`, not SDX55's endpoint PHY and not the PCIe0 x1 table. Base `0x01c16000`, COM `+0`, TX0/RX0 `+0x200/+0x400`, TX1/RX1 `+0x600/+0x800`, PCS `+0xa00`, PCS_MISC `+0xe00`. Apply `sm8250_qmp_pcie_{serdes,tx,rx,pcs,pcs_misc}_tbl` followed by **gen3x2 RC overrides**. The new numeric `umi_modem_phy.h` records these register/value facts; no Linux driver function bodies were copied. Set PCS power-down control `+0xa40` bits 0/1, enable PIPE, clear PCS software reset `+0xa00` bit0, start `+0xa44` bits0/1, wait for `+0xa14` bit6 to clear.
8. Configure DWC root-port x2 / Gen3, hold ASPM disabled for the polling experiment, allow 100 ms power/refclk stability, release PERST#, enable PARF LTSSM bit8 at `+0x1b0`, wait for ELBI link bit10 and DLL-active bit29 at DBI `+0x80`, then allow 100 ms configuration recovery. Configure bus 1 and outbound CFG0 to BDF 01:00.0. Require **vendor/device `17cb:0306`** (`0x030617cb` dword). The `17cb:010c` subsystem ID is also used by flash-backed Foxconn modules; it is insufficient to choose their firmware profile.
9. Keep bus mastering off until an independent bounded SMMU context exists for **SID `0x1d01`**, with root SID base `0x1d00`. Mainline's PCIe patch fixes parsing a **five-cell iommu-map entry** (SID plus mask), validates it before clearing BDF-to-SID bypass, then installs the CRC8/0x07 routing table. The endpoint route is BDF `0x0100`, SID delta1, encoded `0x01000100`. This must not use Wi-Fi's SID `0x1c01`.
10. Linux uses GICv3 MBI, alias **`0x17a10000`**, SETSPI **`0x17a10040`**, INTIDs **864–895** (SPI832–863). DTS comments explain that SBL bypasses the DWC internal MSI receiver. The new diagnostic maps the alias in its SMMU domain, configures endpoint MSI and keeps vectors masked, polling the actual event-ring memory. MSI behavior during SBL requires hardware validation.

### Firmware and transport: exact recovered flow

**Host-loaded, not self-booting flash:** the umi-specific MHI profile selects `qcom/sdx55m/sbl1.mbn`, 32-bit DMA, 32768-byte MRU and a 20-second timeout. PBL EE0 receives the **entire 548056-byte SBL over BHI**, then SBL EE1 runs Sahara over channels 2/3. No BHIE combined image, Firehose, EDL or flash programming is used. Wi-Fi's 512 KiB/BHIE loader cannot be reused unchanged.

The actual successful loader is `work/iter1/tools/sahara-load.c`, invoked by `start-sdx55.sh`. Sahara HELLO modes **0 and 1 must be echoed**; mode2 is a different EFS protocol. Read32 command3 and Read64 command0x12 carry ID/offset/length. Validate the whole range before sending, serve in 32768-byte chunks, assemble fragmented headers and retain coalesced messages. SBL can remove Sahara without a final DONE; transport termination alone is not success. Linux checks live AMSS plus mission devices. HakiOS requires live EE2 and acknowledged IPCR channel starts, and never converts a malformed packet, bad file range or SMMU fault into success.

`work/iter1/stage-firmware.py` reads original `work/backup/20261022/modem.img` with `mcopy` from FAT path `::/image/sdx55m/*`. The other four inputs are original partition backup files, not files from inside that FAT image. Exact working map:

| Sahara ID | File | Origin |
|---:|---|---|
| 6 | apps.mbn | modem.img /image/sdx55m |
| 8 | qdsp6sw.mbn | same; 84004996 bytes |
| 16 | mdm1m9kefs1.img | original mdm1m9kefs1 partition; 8 MiB |
| 17 | mdm1m9kefs2.img | original mdm1m9kefs2 partition; 8 MiB |
| 20 | mdm1m9kefs3.img | original mdm1m9kefs3 partition; 8 MiB |
| 21 | sbl1.mbn | modem.img /image/sdx55m; 548056 bytes |
| 23 | aop.mbn | modem.img /image/sdx55m |
| 25 | tz.mbn | same |
| 29 | acdb.mbn | same |
| 33 | hyp.mbn | same |
| 34 | mdmddr.img | original mdmddr partition; 1 MiB |
| 36 | multi_image_qti.mbn | modem.img /image/sdx55m |
| 37 | multi_image.mbn | same |
| 38 | xbl_cfg.elf | same; vendor map's xbl_config.elf is renamed |
| 40 | apdp.mbn | same |
| 41 | devcfg.mbn | same |
| 42 | sec.elf | same |

All **17 staged files (114843743 bytes)** were verified against `work/iter1/fw/staged-manifest.json`; the 13 signed images were also independently matched byte-for-byte against the original `modem.img`. Use `work/iter1/fw/staged/qcom/sdx55m`. Do not use extracted `efs1/2/3.bin` (512-byte distribution placeholders), extracted `mdmddr.mbn` (40 bytes), AP `modemst1/2`, `fsg`, or `persist` as substitutes. The smaller `/Users/xm666/X/umi-ios/backup` does not contain the required external-modem image set. No `apnhlos` image is used by the recovered loader.

**TFTP provenance distinction:** `work/modem-data/pack-userspace.sh` packages `work/boot1/firmware/extracted/image/modem_pr`. That older firmware directory records a Global V14.0.5.0 firmware package source, whereas the boot/Sahara bundle above explicitly comes from the phone backup. An in-memory comparison of all 338 `modem_pr` files in the successful `deploy.tar.gz` against original `modem.img:/image/modem_pr` found **8 identical, 330 different, none missing**. Preserve the verified archive's local tree for reproducing stage2; silently substituting the partition's carrier files would change the tested recipe. No archive extraction or proprietary copy was made in this task, and none belongs in the patch.

| Function | Channels | ER | Working use |
|---|---:|---:|---|
| Sahara, SBL | 2/3 | 1 | 128 entries; host image transfer |
| DIAG | 4/5 | 1 | 64 entries; unused here |
| EFS, mission | **10/11** | **1** | 64 entries; RAM memory-sync responder |
| QMI WWAN | 14/15 | 0 | 64 entries; exists but ignored by successful MM recipe |
| IPCR/QRTR | **20/21** | **0** | 64 entries; **QRTR node3 is the working control path** |
| IP_HW0 | 100/101 | 4/5 | 128 entries; mhi_net data with QMAP |

Six ERs contain 64,128,64,64,1024,2048 entries, MSI vectors1–6; ER4/5 are hardware rings. No DUN/MBIM channel is used. The HakiOS diagnostic allocates 128 TREs for each opened software channel; that is a host ring capacity, not a different wire channel profile. IPCR ER0 corrects the older Android ER2 assumption and Wi-Fi's ER1.

### Mission services and the successful Linux startup order

1. Run the host firmware loader and establish **AMSS EE2**. The systemd `umi-sdx55.service.d/20-power.conf` then pins root `0002:00:00.0` and SDX55 `0002:01:00.0` runtime power/control to **on**. MHI's MISSION callback resets the setting to auto, so setting it only before loading is insufficient. The unpinned registered boot lost PCIe at uptime108 seconds. HakiOS's diagnostic holds M0, ASPM off and clocks/votes on throughout; production suspend is deferred.
2. Start **mission EFS10/11** with `umi-efs-ram`. This is Sahara **memory-debug mode2**: consume bounded 32/64-bit memory tables, request and discard snapshots, send RESET7 and consume RESET_RESP8 to acknowledge the sync session. RESET here finishes the EFS protocol; it does not authorize writing calibration partitions. Reference bounds: table4096 bytes, 52/64-byte entries, region64MiB, total128MiB, read chunks64KiB. Successful sync consumed a 2MiB region. Seed each boot with the original local EFS images; session NV updates are intentionally lost across reboot.
3. Wait for AMSS/EFS and QRTR **node3**. `umi-wait-modem-transport` waits up to60s for service21 on node3. Only then publish patched **tqftpserv version3** and serve the verified local `modem_pr` tree. Early publication repeatedly left radio offline/DeviceNotReady with no carrier-file reads. TFTP writes go exclusively to `/run/umi-tqftpserv` in RAM; the rest of the service filesystem is read-only. HakiOS needs a bounded in-kernel service or a small daemon plus QRTR transport exposure; a Linux binary cannot simply be copied into iOS.
4. Wait up to120s for mission EFS and QRTR node3 **WDS1/DMS2/NAS3** before starting ModemManager (its initial four-second discovery window is too short). The udev recipe ignores WWAN Sahara/QMI/DIAG and groups `mhi_hwip0` under `qcom-soc`; a plugin patch admits `mhi_net`. Control is qmicli/MM **over QRTR**, not `/dev/wwan0qmi0`.
5. For data, `--test-multiplex-requested` lets the ordinary NM GSM profile allocate **QMAP mux1**, `qmapmux0.0` over `mhi_hwip0`. The verified setup uses APN **cmnet**, IPv4, observed MTU1410, route metric700, DNS priority200. Wi-Fi routes have priority. `sdx55m-data-up-reference.sh` describes a raw-IP alternative (PCIe endpoint interface4); it is not the final successful QMAP sequence. Do not import SC7280 IPA endpoint numbers.

QMI service roles: DMS2 operating mode, NAS3 serving system/signal/registration, WMS5 SMS, UIM11 SIM status, WDS1 sessions/IP settings and WDA26 format negotiation. QRTR uses discovered service ports without QMUX CTL0 allocation. Query real SIM state before slot/PIN actions: the reference reports inconsistent MM slot metadata despite working data/SMS. Read-only first queries can be NAS Get Serving System0x24, NAS Get Signal Strength0x20 and UIM Get Card Status0x2f. Radio-online/registration and WDS session operations follow verified service readiness. No PIN change or forced SIM provisioning is required by the recovered recipe.

Reference patches/tools are under `work/modem-data/{patches,tools,overlay}`; pinned revisions in `sources.json`: ModemManager `dfa41adf391b090720fb1ea56d884f61ea7fba29`, modem-userspace `a66ce4c045a4311ff27c6024f7a7d6baf3eaf2c9`, tqftpserv `43ecec1b5dcf28fdf06fd5dc5c1b37686a9384a4`. They are protocol references; new HakiOS code does not paste GPL implementation blocks.

### HakiOS mapping and implementation

Upstream `../oosexgang_hakios-xnu-kernel` and this tree's `drivers/modem` provide QMI TLV/transaction helpers, `hakios_qmi_handle_init_transport`, NAS parsing, cellular sysctls and SpringBoard hooks. They are useful above a new transport. `hakios.mpss.c` is monolithic integrated-MPSS boot/SMEM/SMP2P/GLINK/RMTFS/IPA setup; its PAS4 and RAM assumptions are not used. `hakios.ipa.c` implements SC7280 IPA4.11, not Kona APQ IPA4.5, and is not used. Upstream itself documents LTE registration but an unresolved packet-domain/data-session problem; its implementation is not evidence of a working umi data plane.

`hakios: cellular: aggregator methods` comes from `userland/src/inject_sb.c` discovering/hooking SpringBoard methods. It is **not modem detection or registration**. `kern.hakios.cellular_{up,registered,bars,dbm,radio,operator}` can later feed those hooks after real NAS results. CommCenter/job shims and the `en1` Ethernet/DHCP adapter do not provide an Apple baseband driver. This patch deliberately does not fabricate cellular-up/bars state.

The reusable PCIe/MHI experience is in `devices/xiaomi-mi-10/overlays/xnu/files/bsd/dev/{umi_pcie,umi_mhi,umi_wifi,hakios.wifi,hakios.lx}.c`; the lx/ath11k stack is the Wi-Fi client. The EFI loader validates boot arguments and loads the kernel; it is not the modem PCIe/MHI driver. Wi-Fi's single-context allocator, DMA range, event ring and firmware flow cannot simply be shared with X55. New modem files implement a private PCIe2 domain and protocol state, preserving the same checked MMIO, DMA barriers, command-completion and polling approach.

New files: `umi_modem.c`, `umi_modem_{pcie,dma,mhi}.inc`, `umi_modem_phy.h`, `umi_modem_power.inc`, native driver wrapper, firmware staging helper, tests. Minimal hooks: driver `conf`, exact loader whitelist tokens, UMI input initialization replaces integrated `hakios_mpss_init`, the existing Wi-Fi wrapper's final quiesce hook also asserts X55 PERST, one include in RPMh, and optional system-image firmware staging. No common/upstream driver templates were edited.

Boot modes:

- **`umi_modem_probe=1`**: rails/GPIOs/clocks/PHY/link, enumeration and BHI EE read with **bus mastering off**, then assert PERST. No firmware required.
- **`umi_modem=1`**: private SMMU SID0x1d01, full BHI SBL, bounded Sahara, AMSS, post-AMSS M0/EE-event synchronization, acknowledged IPCR20/21 ER0 with a pre-queued DL buffer, and QRTR HELLO/NEW_LOOKUP with service discovery. Stage3 PASS requires HELLO plus NEW_SERVER announcements; the modem remains live until loader-return quiesce.

Both are default off and may run with the full Wi-Fi driver set. Exact tokens are added to loader `allowed[]`; `modem=1`, `umi_modem=2` and arbitrary modem/soc overrides remain rejected. The startup hook replaces UMI's integrated `hakios_mpss_init`. There is a 240-second active-protocol deadline, per-operation timeouts, checked image ranges and event pointers, and no recovery loop. Failure retains DMA memory/domain until reboot and asserts PERST; the loader-return/panic hook also asserts PERST before kernel memory reuse. Shared RPMh votes are never lowered. No status-bar state is fabricated.

**Coexistence:** PCIe0/1/2 share GCC `0x6f014` (`gcc_pcie_phy_refgen_clk_src`). The original UMI Wi-Fi path selected 19.2 MHz while PCIe2 requires 100 MHz, which caused the first probe guard to reject `wifi=1`. The Wi-Fi PCIe path now selects the SM8250 DTS rate of 100 MHz too; a shared refgen lock makes the rate programming atomic. If boot firmware left the shared source at 19.2 MHz, either path repairs it to 100 MHz while no path is changing the source concurrently. Modem PCIe2 no longer touches PCIe0's AUX RCG (`+0x6b038`). PCIe0 and PCIe2 retain separate controllers, BARs, SMMU streams (`0x1c01` and `0x1d01`), contexts, and MSI mappings. Bluetooth rail sharing uses the existing RPMh client/sequencer locks and the same voltage minima; the patch does not drop its votes.

**Implemented:** stage3 IPCR/QRTR diagnostic and private firmware validation/staging, including the recovered 17-image map. **Not implemented:** EFS responder, TFTP daemon, persistent QRTR socket API, NAS/WMS clients, WDS/QMAP/iOS interface, PON reset, runtime PM and crash recovery. Mission EFS contexts are described but not opened/served yet. No stage3 hardware success is claimed by compilation. No firmware is present in the patch or newly built system image; the optional system-image hook must be used before a full hardware test.

### Supervisor hardware test plan

No phone commands below were executed. Use the new kernel **and loader**, a fresh boot for each experiment, and the existing supervisor capture workflow. Keep the complete daily driver set, including `wifi=1`; add only the exact external-modem token. A loader-return run is not a guaranteed modem PMIC reset; a non-PBL initial EE intentionally stops the full path.

1. **Default-off regression:** ordinary baseline boot arguments. No `UMI MODEM: start` line. Check UI/touch/Wi-Fi/BT as before.
2. **Probe, exact complete arguments:**

   ```text
   -v bootcpu=0 cpus=8 umi_touch=1 umi_keys=1 umi_modem_probe=1 loader_return_after=45
   ```

   Ordered expected log substrings (variable fields follow the shown prefix):

   ```text
   UMI MODEM: start external-X55 mode=probe; integrated MPSS disabled
   UMI MODEM: rails ldoa5=880 ldoa9=1200 errno=0
   UMI MODEM: power inherited firmware-established PON; AP_STATUS=1 AP_ERRFATAL=0
   UMI MODEM: clocks ready GDSC=on AUX=19200000 REFGEN=100000000
   UMI MODEM: PHY ready QMP-v4 Gen3x2
   UMI MODEM: link up status=
   UMI MODEM: PCI ID=0x30617cb expected=0x030617cb
   UMI MODEM: enumerated BAR0=0x64300000 size=
   UMI MODEM: EE=0 (0=PBL 1=SBL 2=AMSS 3=RDDM 4=WFW 6=EDL)
   UMI MODEM: PASS probe PCI=17cb:0306 EE=0 DMA=off
   UMI MODEM: quiesced PERST asserted; shared votes and DMA retained
   ```

   Probe may report another valid EE; record it, but full stage1 requires PBL0. A stopped log after a clock/register stage needs last-access diagnosis: unclocked SM8250 MMIO can hang the bus. Do not improvise PMIC writes or try MPSS.
3. **Offline firmware preparation:** the new importer defaults to validation only. A writable, new destination under the requested work directory is needed for copying:

   ```sh
   python3 devices/xiaomi-mi-10/tools/import-modem-firmware.py \
     --reference /Users/xm666/X/mainline/umi-linux \
     --target /Users/xm666/X/umi-ios/work/modem-fw
   ```

   It copies exactly17 verified local files, writes the supplied `sahara.map` and `SHA256SUMS`, and protects the directory/files as0700/0600. No TFTP files are needed for stage3 control-plane discovery. During the ordinary **system** APFS build set `UMI_MODEM_FW_DIR=/Users/xm666/X/umi-ios/work/modem-fw`; the optional overlay hook validates every hash and stages `/fw/sdx55`. Source `env.sh` and wrap the build in `build-lock.sh`. Do not use integrated `MODEM=1` packaging or place the114.8MB bundle in the small loader payload. Recheck APFS placement/capacity after rebuilding the system image.
4. **Stage3 control-plane, exact complete arguments:**

   ```text
   -v bootcpu=0 cpus=8 umi_touch=1 umi_keys=1 wifi=1 umi_modem=1 loader_return_after=300
   ```

   After probe progress, require:

   ```text
   UMI MODEM: SMMU SID=0x1d01 CB=
   UMI MODEM: protected DMA enabled; MSI alias=0x17a10040 INTID=864 masked polling
   UMI MODEM: BHI SBL accepted bytes=548056
   UMI MODEM: MHI M0 contexts ready events=6
   UMI MODEM: channel open id=2 ER=1
   UMI MODEM: channel open id=3 ER=1
   UMI MODEM: Sahara HELLO image-mode=
   UMI MODEM: Sahara image complete id=
   UMI MODEM: EE=2 AMSS
   UMI MODEM: mission power held on; runtime suspend disabled
   UMI MODEM: channel open id=20 ER=0
   UMI MODEM: channel open id=21 ER=0
   UMI MODEM: QRTR discovery services=
   UMI MODEM: MHI M0 after AMSS EE event
   UMI MODEM: QRTR HELLO received
   UMI MODEM: QRTR service=
   UMI MODEM: PASS stage3 AMSS IPCR=20/21 ER=0 QRTR control channel open
   UMI MODEM: quiesced PERST asserted; shared votes and DMA retained
   ```

   Before AMSS, either `Sahara complete bytes=` or `Sahara transport ended errno=...; verifying AMSS/IPCR` is allowed. The latter is **not** PASS by itself. Stage3 does not start data calls; the modem remains live after PASS and is quiesced by the loader-return hook.
5. **Failure:** any `UMI MODEM: FAIL stage=<name> errno=<n>`, `timeout stage=`, `MHI/SMMU fault`, or missing final PASS. Missing `/fw/sdx55/sahara.map` is an expected firmware-stage failure before DMA. A pre-owned SID is left intact. The shared REFGEN is deliberately normalized to 100 MHz under the PCIe lock because that is the rate required by all SM8250 PCIe PHY consumers. No automatic retry follows failure. Preserve a private full log; redact identifiers before sharing.
6. **Later stage2 acceptance:** keep mission alive past108s, complete bounded EFS discard/ack, publish TFTP3 only after node3 readiness, observe carrier-file reads then WDS1/DMS2/NAS3 on node3. Test an attempted TFTP write reaches only RAM, traversal is rejected and no calibration partition changes. Stage3 requires real NAS registration/signal and an explicitly authorized SMS test. Stage4 requires mux1 session/IP settings plus interface-bound IPv4 traffic through an iOS network interface. These are future criteria, not current log promises.

### Build validation and delivery

Kernel and EFI loader built after sourcing `/Users/xm666/X/umi-ios/env.sh`, serialized by `build-lock.sh`. `prepare.py verify` passes. Hardware-free tests exercise actual PCIe/MHI code with mocked MMIO/VFS: domain/clock sequencing, refusal before unclocked access, link/PHY timeouts, PCI identity/BAR bounds, DMA-off probe, event pointer/completion validation, complete image-range rejection, 84MB images, HELLO0/1, fragmented/coalesced messages, live-AMSS handoff, exact loader whitelist and checksum/path validation. These establish software checks, not electrical or modem interoperability.

The sandbox permits writes only inside this repository and temporary directories. The requested outer report/patch/firmware destinations are not writable in this session. A complete updated report and isolated patch are staged under **`REPO/out/modem/`**, with source documentation here. Firmware remains read-only in the reference; import it into the requested private directory when run in a writable supervisor environment. The patch contains only modem-owned files/hunks; other agents' work and proprietary material are excluded.

### Revised stages and estimate

With the complete verified recipe, regular supervisor test cycles and the firmware-established PBL prerequisite holding:

| Stage | Concrete milestone | Engineering estimate from this handoff |
|---|---|---|
| 1 | Validate PCIe2/PHY/SMMU, BHI/Sahara, AMSS and IPCR on hardware | 2–5 working days |
| 2 | Persistent QRTR plus EFS RAM responder and late TFTP3 | another3–7 working days |
| 3 | Real NAS signal/registration, bars integration and WMS SMS | another3–7 working days |
| 4 | WDA/WDS, QMAP mux1, iOS interface/routing | another1–3 weeks |

Plan **2–4 weeks to real signal bars and SMS**, **3–7 weeks cumulative to initial IPv4 data**, versus6–12 weeks to bars without a reference. The improvement is known boot images, proven control path, exact EFS/TFTP ordering and a validated data recipe. Remaining uncertainty is the XNU PCIe/SMMU/MHI implementation and boot power handoff, not whether Linux can use this hardware. PON reset, Wi-Fi clock coexistence, suspend/recovery and iOS network-policy integration can extend this. Voice/IMS remains outside this estimate. No schedule assumes hardware has already passed.

## stage3 IPCR/QRTR

The mission transition waits for both MHI state M0 and the EE=2 change event before touching the client channels. IPCR UL20 and DL21 use event ring 0, matching the SDX55 Linux profile. UL20 is started first; DL21 is started with its RX TRE already posted, matching Linux `auto-start` plus `auto-queue`. QRTR HELLO and NEW_LOOKUP are sent over UL20, and DL21 is polled for control packets.

Stage3 is complete when the log contains the QRTR HELLO acknowledgement and NEW_SERVER announcements (`service`, `instance`, `node`, `port`) followed by the PASS line. It intentionally stops at control-plane discovery: no EFS responder, QMI data call, TFTP write, QMAP setup or subscriber identifier logging is added. On success the modem stays up for the remainder of the run. The loader-return quiesce hook then stops MHI, asserts PERST and retains the DMA domain until reset.

Expected phone lines:

```text
UMI MODEM: EE=2 AMSS
UMI MODEM: MHI M0 after AMSS EE event
UMI MODEM: channel open id=20 ER=0
UMI MODEM: channel open id=21 ER=0
UMI MODEM: QRTR HELLO received
UMI MODEM: QRTR service=
UMI MODEM: QRTR discovery services=
UMI MODEM: PASS stage3 AMSS IPCR=20/21 ER=0 QRTR control channel open
UMI MODEM: quiesced PERST asserted; shared votes and DMA retained
```
