# 调试方法论

这份文档记录这个移植项目里"怎么查问题"，以及踩过的坑。目的是下次卡住时不用重走弯路。

## 判死亡层级：先看哪个定时器动了

启动链每一层都有可观测的副作用，按顺序排列：

| 层级 | 副作用 | 判据 |
| --- | --- | --- |
| ABL | `fastboot boot` 返回 OKAY | 命令成功 = 镜像被接受 |
| BootShim | 把 payload 卷拷到 0xB0000000，写交接页 | 无独立观测（要看固件日志） |
| UEFI 固件 | 屏幕第 0 行 `RDH:<handoff> FDT:<0/1> IRD:<start>+<size> FAT:<0/1>` | 有这行 = RamDiskDxe 跑到了；`FAT:1` = payload 卷已发布 |
| UniXNU 菜单 | 屏幕列表 + 30 秒倒计时 | 有菜单 = 固件完整起来了 |
| DarwinBoot | 加载 kernel.macho / .adt / .trustcache | 无独立观测 |
| XNU 早期 | 屏幕上的 `boot-progress` 条形 | 有进度条 = 内核在跑 |
| XNU launchd | `miro-autoreset=<秒>` 到点 PSCI 复位回 fastboot | **回 fastboot = 内核跑到了 launchd** |
| 崩溃 | 写 oops 环 + ramoops panic 记录 | reader 能读到 |

**最快判据**：`miro-autoreset` 是内核里的定时器。**boot 后 n 秒不回 fastboot，就说明内核没跑到 launchd**——这一条把"内核问题"和"内核之前的问题"干净切开，不需要任何日志。

## 静态验证的边界

改名/重构这类"应该等价"的改动，可以逐层验证等价性，不必上机：

1. **源码层**：用 `difflib` 对两个树做 diff，过滤掉 `dada→miro` 替换后，剩余 hunk 数应为 0。
2. **符号层**：`llvm-nm` 对比两个 Mach-O 的符号表；按改名映射过滤后，两侧应一一对应（地址也相同）。
3. **段结构**：`LC_SEGMENT_64` 逐段对比 vmaddr/vmsize/filesize。
4. **boot image 组装**：解 gzip 后按 `shim + firmware + payload` 切分，对比 shim 的 ARM64 头（base/total）、FW 的 `_FVH` 签名、payload 的 BPB 字段。
5. **FAT 内容**：直接解析 BPB 和目录项，逐文件比对大小与哈希。

这次五层都验证通过，说明**改名本身没有破坏任何东西**。

## 坑 1：压缩容器必须解压后再验证

**FD 固件卷是 LZMA 压缩的。** 用 `grep` / `strings` 查 `.fd` 文件，看到的是压缩字节流——里面什么字符串都搜不到，会得出"固件里没有设备标识"的错误结论。

正确做法：

```python
import uefi_firmware
blob = b"".join(walk(uefi_firmware.AutoParser(open(fd, "rb").read()).parse()))
print(blob.count(b"DADA2ESP"))   # 解压后才能看到
```

这个坑让根因（`UniXNURamDiskDxe` 里的 `ESP_LABEL "DADA2ESP"`）多藏了五轮排查。

## 坑 2：改名要覆盖所有树

这个项目有五棵树，改设备标识必须全部覆盖：

| 树 | 路径 | 内容 |
| --- | --- | --- |
| 设备仓库 | `UniXNU-miro` | board 配置、驱动、内核补丁、包 |
| UEFI fork | `uefi-miro` | Mu-Silicium + `Platforms/Xiaomi/miroPkg` |
| **UniXNU 基础** | `UniXNU` | **`package/UniXNUPkg/`（RamDiskDxe 等）+ `scripts/`** |
| SoC 共享 | `UniXNU-sm8750-common` | SM8750 公共资源 |
| reader | `UniXNU-miro-linux` | 调试工具链 |

漏掉第三棵树的代价：payload 卷标改了、找卷的代码没改，整条链在 UEFI 阶段静默失败。

## 坑 3：`grep dada2` 会漏掉纯大写

`DADA2ESP` 是小写 `dada2` 的**大写形式**，`grep -i` 能匹配，但 `grep dada2` 不能。审计时用大小写不敏感的模式，并单独扫一遍全大写变体。

## 坑 4：区分"契约"和"标识"

不是所有旧名字都该改：

**该改（标识）**：boot args（`miro-log`）、DTS compatible（`xiaomi,miro`）、payload 卷标（`MIROESP`）、平台包名（`miroPkg`）、驱动名（`MiroUfs`）。

**不该改（契约）**：

- 介质上的 magic：`OOPSLOG1`（槽头）、`DADAPNC1`（panic 记录）、`DADAPRB1`（探针）——它们是**驱动写、reader 读**的双方契约，改了要两边同步，且旧数据会读不出来。
- 上游集成符号：`XNU_SHENG_PMGR`、`sheng_original_pmgr_nub_*`、`sheng-pmgr=2`——这些是 UniXNU 基础层的接口，改了会破坏基础补丁。
- vendor 二进制路径：`Binaries/dada/`——那是上游小米 15 的固件分支名，不是我们的标识。

**判据**：这个字符串的**另一端**在谁手里？如果另一端是磁盘上的旧数据、上游代码、或第三方二进制，就不能单方面改名。

## 坑 5：并发脚本会互相抢设备

多个 watcher 同时轮询 `fastboot devices` 并尝试 `fastboot boot`，会互相干扰——一个脚本 boot 了 XNU，另一个立刻 boot reader，看起来像"手机莫名奇妙死了"。

跑自动化时**只留一个** watcher，并在启动新脚本前 `pkill` 旧的（注意 `pkill -f` 可能匹配到自己所在的 ssh 命令行，把会话杀掉）。

## 坑 7：定长字段与字符串字面量

FAT 卷标是 **8 字节、空格填充** 的定长字段，而 C 字符串字面量会在末尾附一个 `\0`。这两件事一旦相遇，长度恰好差一就静默失败：

```c
CompareMem (Bpb + 0x47, ESP_LABEL, 8)     // 固定读 8 字节
```

| 字面量 | 字面量内容 | 第 8 字节 | 匹配 `MIROESP␣␣␣␣` |
| --- | --- | --- | --- |
| `"DADA2ESP"` | 8 字符 + `\0` | `S` | 是（恰好 8 字符） |
| `"MIROESP"` | 7 字符 + `\0` | `\0` | **否** |
| `"MIROESP "` | 8 字符 + `\0` | 空格 | 是 |

从 `DADA2ESP` 改名到 `MIROESP` 时，标识符少了一个字符，比较就永远不成立。**改名前是"刚好能用"的状态，改名把隐藏的宽度假设暴露出来。**

这个 bug 的表现是固件静默退回内置配置：菜单只剩 `Return to Firmware / Restart / Power off / Tools`，屏幕角落一行 `built-in ... f0`（`f0` = FAT 卷校验失败），`drivers 0 loaded`。看起来像更早期的固件版本，实际是"找不到 payload 卷"的 fallback。

修法不只是补空格，还要让编译器守住这个不变量：

```c
#define ESP_LABEL "MIROESP "
STATIC_ASSERT (sizeof (ESP_LABEL) - 1 == 8, "the label must fill the 8-byte BPB field");
```

**通用判据**：凡是 `memcmp` / `CompareMem` 带显式长度的地方，检查两侧的宽度从哪来——定长字段（BPB、分区表、UUID、magic）和 C 字符串字面量不是同一回事。改名字、改常量、改配置时，先看有没有 `sizeof` / 定长比较依赖它。

同类陷阱：`StrCatS` 的容量参数、`snprintf` 的缓冲区边界、协议头的固定字段。

## 坑 6：reader 必须刷 DTBO

reader 的无 DTBO 启动会卡在 UFS 初始化（屏幕停在 `UFSDBG:` 序列），USB 永远起不来，主机侧看到的是 "No route to host"——很容易误判成"手机关机了"。

`auto.sh` 每轮都刷 `dtbo_b`；手写拉取脚本时**必须包含**：

```sh
fastboot --set-active=b
fastboot flash dtbo_b out/boot-miro-v33/dtbo.img
fastboot boot out/boot-miro-v32/boot.img
```

## 取证通道优先级

1. **oops 环**（UFS 分区，16 MiB）：内核起来之后的完整控制台。需要 reader 读。
2. **pstore ramoops**（`0xa3500000`，4 MiB）：panic 文本，warm reset 后仍在。reader 一启动就能读，但 **reader 自己的 console 会覆盖它**，所以要在 reader 启动后立刻读。
3. **屏幕**：固件菜单、`RDH:` 状态行、`boot-progress` 条、panic screen。任何 UEFI/内核早期问题只能靠它。
4. **netconsole**：内核起来但 UFS 还没起来时的唯一通道。
