# AGENTS.md

给在这个仓库里工作的代理和贡献者的操作说明。读 `README.md` 了解项目状态，读 `porting.toml` 了解每个领域的记录。

## 构建循环

构建机是 Linux 主机，UniXNU 主仓库在同级目录（`../UniXNU`）。

```sh
cd UniXNU-miro
export UNIXNU_KERNELCACHE=<主仓库>/out/ipsw/21H461/kernelcache.decompressed
../UniXNU/unixnu kernel -D ../UniXNU-miro -j4        # 内核 + prelink
../UniXNU/unixnu payload -D ../UniXNU-miro --unique-chip-id <hex>   # 需要 ECID 种子
../UniXNU/unixnu bootimg -D ../UniXNU-miro --fd <SILICIUM_UEFI.fd> --dtb <dtb>
```

- `--unique-chip-id` 只走命令行，任何设备标识都不进仓库。
- SoC 源走 `local.toml`（见 `local.toml.example`）；`feeds.toml` 里的是待发布 pin。
- 内核产物在主仓库 `out/miro/ipados-17.7.11-21H461/miro/` 下。

## 内核补丁

`patches/xnu/` 是本机型自己的补丁，按 `patches/xnu/series` 的**逐行顺序**应用（不按文件名排序，名字只是语义标签）；arch / SoC / OS profile 三层来自各自的仓库，由 `unixnu` 驱动拼装，不要在设备仓库里复制基础补丁。

| 补丁 | 覆盖内容 |
| --- | --- |
| `01-amfi-bringup-guards.patch` | 无 AMFI 预链接时的空指针防护、root 鉴权门、entitlement 加速、`miro-no-cs` 签名放行 |
| `02-owner-cache-path.patch` | owner cache / dyld 共享缓存路径计算 |
| `03-owner-cache-bringup.patch` | owner cache 带启动、dyld 诊断探针、崩溃记录、critical daemon 定时复位 |
| `04-ufs-console-log.patch` | 内核 printf 控制台环、oops 快照（`MIROLOG2` / `DADAPNC1` 格式）、panic hook |
| `05-launchd-job-policy.patch` | 去掉 `XNU_BOOT_PROGRESS` guard，让 `sheng-disable-jobs` 策略生效 |
| `06-crash-registers.patch` | 在 `proc_prepareexit` 把崩溃进程的 sig / PC / LR / FP / bt 打到控制台 |

改内核的流程：从 `UniXNU/out/feeds/<xnu-rev>/` 取原始文件，在临时 git 仓里按顺序应用 `series` 全部补丁，改完用 `git diff` 生成新补丁——**hunk 头一律由 git 生成，不要手数行数**。也可以直接在构建树 `.cache/xnu-work` 上验证改动，再按同样方式回写补丁。

## 已知的坑

- `.config` 是 defconfig 的快照。改配置要改 `configs/miro_defconfig`，然后重新跑 `unixnu defconfig`。历史教训：只在 out 树里改 `.config`，下次重建就丢。
- 构建树 `tree/scripts/` 里的脚本是从本仓库复制的。要改脚本就改本仓库的，树里的副本下次组装会被覆盖。
- `CONFIG_XNU_OWNER_CACHE_STARTUP` 必须为 `y`（BOOT_MANIFEST 依赖它）。要做"关闭 ignition"的对照实验，用 `miro-dyld-ignition=1` 启动参数，不要动这个配置。
- 平台电源驱动的 sheng 系符号（`sheng_original_pmgr_nub_*`、`XNU_SHENG_PMGR`、`sheng-pmgr=2`）是上游基础层的集成点，重命名会破坏基础补丁的语义。驱动自身的类名、文件名、产物名走 `Miro*`。

## 调试循环

崩溃取证不拍照：内核 panic 写 `oops` 分区（16 MiB，未加密），PSCI 定时复位回 fastboot，Linux reader 镜像把分区拉回来解析。reader 和无人值守脚本在 [UniXNU-miro-linux](https://github.com/Nanawwa/UniXNU-miro-linux)。日志格式见 `04-ufs-console-log.patch` 和 reader 的解析脚本，两边必须同步改。

启动参数速查（完整表在 `board.toml`）：`miro-log=1` 开日志环；`miro-data-persist=2` 开 Data 存储窗口；`miro-no-cs=1` 是带启动用的签名放行，正式方案是 StaticTrustCache；`miro-autoreset=<秒>` 定时回 fastboot；`miro-dyld-ignition=1` / `miro-dyld-verbose=1` 控制 dyld ignition 与诊断。

## 安全边界

- 一切验证用 `fastboot boot`（RAM），不写分区。
- 任何分区写入：先备份、有回滚件、经过机主明确同意。A/B 两个槽都要考虑。
- 不碰 userdata，不为修 Android 动分区表。
- 提交前检查：序列号、ECID、本机绝对路径不进仓库。
