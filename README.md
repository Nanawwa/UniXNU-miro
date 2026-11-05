# UniXNU-miro

Device support for the Redmi K80 Pro (codename `miro`, Qualcomm SM8750) on [UniXNU](https://github.com/UniXNU/UniXNU), which boots iPadOS on non-Apple ARM64 hardware.

`fastboot getvar product` reports `miro`. The port boots the iPadOS 17.7.11 (21H461) kernel through Mu-Silicium UEFI. Images load over `fastboot boot` and live in RAM only — nothing is written to the phone, and a reset returns it to Android.

This repository descends from the Xiaomi 15 port ([7Night-kqx/UniXNU-dada](https://github.com/7Night-kqx/UniXNU-dada)); both devices share the SM8750. The shared-SoC work (UFS transport, CPU topology, timer, GIC) carries over; this repository owns the board-specific identity, memory map, partition windows and payload policy.

## Status

- Kernel boots on the phone: UEFI menu → DarwinBoot → XNU → UFS root mount → launchd.
- Storage works: UFS root volume, an 8 GiB Data container, and a 16 MiB unencrypted `oops` partition that carries the console ring for crash forensics.
- Open problem: userland ignition stops at dyld's shared-cache mapping. The cache directory reaches launchd; dyld still does not map the shared region. That is the active work item.
- Display, input, GPU, power management: not started. The panel is 1440x3200; first-boot memory reservations are conservative.

## Layout

| Path | Contents |
| --- | --- |
| `board.toml` | Board configuration: components, archive plan, payload policy, ADT tables |
| `configs/miro_defconfig` | Kernel configuration for this board |
| `dts/miro.dts` | Memory map, reservations, timer, ADT exports |
| `drivers/MiroUfs` | SM8750 UFS transport (fork of dada's DadaUfs) |
| `drivers/MiroPlatformPower` | PMGR instrument (reuses sheng's, addressed for this kernelcache) |
| `package/` | UEFI-side components: boot log, boot progress, diagnostics |
| `patches/xnu/` | This board's XNU patch series (see `patches/xnu/series`) |
| `porting.toml` | Per-area status, evidence and next steps |
| `docs/` | Pinned values, UFS notes, handoff, main-repository patches |

## Build (Linux host)

The kernel step uses the UniXNU Linux toolchain.

```sh
git clone --recurse-submodules https://github.com/UniXNU/UniXNU
git clone --recurse-submodules https://github.com/Nanawwa/UniXNU-miro
git clone https://github.com/IssacL04/UniXNU-sm8750-common   # public SM8750 mirror

cd UniXNU
sudo scripts/linux/setup-toolchain.py
./unixnu defconfig miro_defconfig -D ../UniXNU-miro
./unixnu kernel -D ../UniXNU-miro -j$(nproc)
```

The SM8750 source resolves through `local.toml` (see `local.toml.example`) until the maintainer's repository is published; `feeds.toml` records the intended pin.

The payload step needs a unique chip id for the device tree — pass a 64-bit hex value for your own unit:

```sh
./unixnu payload -D ../UniXNU-miro --unique-chip-id <64-bit-hex>
./unixnu bootimg -D ../UniXNU-miro --fd <SILICIUM_UEFI.fd> --dtb ../uefi-miro/Resources/DTBs/miro.dtb
```

The firmware volume comes from a Mu-Silicium fork carrying `Platforms/Xiaomi/miroPkg`; its publication is pending. Until then, `bootimg` needs an FD built from that fork.

## Boot and recover

```sh
fastboot boot boot.img
```

Kernel arguments live in `board.toml` (`[payload] kernel_args`). Diagnostic rounds add `miro-log=1` and the boot returns to the bootloader after `miro-autoreset` seconds. A crash writes the console ring to the `oops` partition, which a Linux reader image reads back over USB — see [UniXNU-miro-linux](https://github.com/Nanawwa/UniXNU-miro-linux) for the reader and the unattended loop.

## License

Same license as the upstream UniXNU device repositories; see `LICENSE`. The UFS transport and PMGR instrument descend from the sheng and dada ports and keep their notices.
