# 移植现场交接（miro）

本文档是连续工作会话之间的交接。约定：设备标识（序列号、ECID）和本机绝对路径不进仓库；构建时需要的 `--unique-chip-id` 由操作者在自己的机器上提供。

## 当前状态

- 内核在 Redmi K80 Pro（miro，SM8750）实机上跑到 launchd：UEFI 菜单 → DarwinBoot → XNU → UFS root 挂载 → launchd 启动。
- 用户态 ignition 停在 dyld 共享缓存映射：`DYLD_SHARED_CACHE_DIR` 已通过环境向量送达 launchd（内核侧 `MIRO-ENV` 诊断行可确认），但 dyld 仍不映射共享区（`vm_shared_region_start_address()` 连续失败）。这是当前主战线。
- 已排除：ignition 找不到 preboot 卷（已通过 owner cache 路径 + union 组合解决）；环境向量断链（l34 修复为顺序填充、终止符置尾，l35 加了内核侧打印）。
- 待验证疑点：`MIRO-ENV` 显示挂载点为 `/` 时缓存路径出现双斜杠（`//System/...`），需要确认 dyld 是否因此拒载，若是则在内核侧归一化。

## 仓库与入口

| 位置 | 内容 |
| --- | --- |
| 主仓库 `../UniXNU` | UniXNU 驱动 + 上游补丁（本地有少量未上游化修改） |
| 设备仓库（本仓库） | board 配置、驱动、内核补丁、脚本 |
| SoC 共享 | `feeds.toml` 记 pin（待发布）；本地用 `local.toml` 指向 IssacL04/UniXNU-sm8750-common 的克隆 |
| UEFI fork（本地） | Mu-Silicium fork，`Platforms/Xiaomi/miroPkg`；未发布，固件构建需要它 |
| reader 仓库 | [UniXNU-miro-linux](https://github.com/Nanawwa/UniXNU-miro-linux)：SM8750 mainline Linux + USB-NCM + dropbear，读 `oops` 分区 |

## 构建

```sh
cd UniXNU-miro
export UNIXNU_KERNELCACHE=<主仓库>/out/ipsw/21H461/kernelcache.decompressed
../UniXNU/unixnu kernel -D ../UniXNU-miro -j4
../UniXNU/unixnu payload -D ../UniXNU-miro --unique-chip-id <hex>
../UniXNU/unixnu bootimg -D ../UniXNU-miro --fd <SILICIUM_UEFI.fd> --dtb <Resources/DTBs/miro.dtb>
```

- FD 与 DTB 来自 UEFI fork 的构建产物（`Build/miroPkg/DEBUG_CLANGPDB/FV/SILICIUM_UEFI.fd`）。
- 全新构建树首次组装约十几分钟；`unixnu kernel` 会因补丁哈希变化自动重建 xnu-work。

## 启动与复位

`fastboot boot boot.img`（仅 RAM，不写分区）。boot.img 装进手机后：

- 崩溃时内核把控制台环写进 `oops` 分区（16 MiB，未加密），然后 PSCI 复位。
- 正常启动 150 秒后（`miro-autoreset=150`）自动回 fastboot，方便无人值守循环。
- 回到 fastboot 后由 reader 循环接手：读 oops → 解析 → 重启到 fastboot，全程不需要碰手机。

## 启动参数

| 参数 | 作用 |
| --- | --- |
| `miro-log=1` | 打开日志环（oops 分区的日志窗口） |
| `miro-data-persist=2` | 打开 Data 存储窗口（8 GiB 种子容器） |
| `miro-no-cs=1` | 带启动：exec 门禁放行（正式方案是 StaticTrustCache） |
| `amfi_get_out_of_my_way=1` | 必带；`cs_enforcement_disable` 会 panic（AMFI 断言），不要加 |
| `miro-autoreset=<秒>` | launchd 起来后定时回 fastboot |
| `miro-dyld-ignition=1` | 抑制 dyld ignition（对照实验用） |
| `miro-dyld-verbose=1` | 向 launchd 环境注入 DYLD_PRINT_*，输出进控制台环 |

## 从 dada2 迁移过来的注意点

标识已全面改名：`dada2` → `miro`，启动参数 `dada-*` → `miro-*`，日志/崩溃记录 magic `DADALOG2`/`DADAPNC1`/`DADAPRB1` → `MIRO*`，UEFI 平台包 `dada2Pkg` → `miroPkg`，payload 卷 `DADA2ESP` → `MIROESP`。**改名前构建的旧镜像仍认 dada-\* 参数**；新镜像与新 reader 配套。驱动保留了 sheng 系集成符号（`XNU_SHENG_PMGR`、`sheng_original_pmgr_nub_*`），那是上游基础层的接口，不是遗留命名。

## 下一步

1. 验证 `miro-dyld-verbose=1` 下 dyld 是否输出诊断（l34 修过环境向量的断链问题，但日志里还没见过 DYLD_PRINT 输出）。
2. 排查双斜杠路径疑点；必要时在内核侧把挂载点拼接归一化。
3. dyld 通了之后：SpringBoard / backboardd 常规用户态问题，回到拍照与 reader 双通道。
4. 显示与触摸移植（1440x3200 面板）在用户态可用后开工。
