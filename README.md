# UniXNU-miro

Device support for the Redmi K80 Pro (codename `miro`, Qualcomm SM8750) on [UniXNU](https://github.com/UniXNU/UniXNU), which boots iPadOS on non-Apple ARM64 hardware.

`fastboot getvar product` reports `miro`. The board boots the iPadOS 17.7.11 (21H461) kernel: Mu-Silicium UEFI → DarwinBoot → XNU → UFS root → launchd. Images load over `fastboot boot` and live in RAM — nothing is flashed, and a reset returns the phone to fastboot.

The port descends from the Xiaomi 15 port ([7Night-kqx/UniXNU-dada](https://github.com/7Night-kqx/UniXNU-dada)); both devices share the SM8750, so the shared-SoC work (UFS transport, CPU topology, timer, GIC) carries over and lives in its own repository. This repository owns the board identity, memory map, partition windows and payload policy.

## Status

- Kernel reaches launchd on the phone; the UFS root mounts and the 8 GiB Data container opens.
- Open problem: dyld does not map the shared cache, so userland stops right after launchd. `vm_shared_region_start_address()` fails every exec; the cache directory itself reaches launchd.
- Display, touch, GPU, power management are not started. The panel is 1440x3200; the firmware GOP covers early output.

## Boot chain

```
Android bootloader (ABL)
  └─ fastboot boot boot.img            # RAM only; Android header v4, gzip kernel, DTB appended
       └─ Mu-Silicium UEFI (miroPkg)   # EDK II firmware volume at 0xA7100000, GOP framebuffer
            └─ UniXNU menu → DarwinBoot
                 └─ BootShim → XNU kernelcache (prelinked against the stock cache, slide -0x1000000)
                      └─ MiroUfs mounts the root volume from the `linux` GPT entry
                           └─ launchd (stock iPadOS cryptex, union-mounted over /)
```

## Tech stack

| Layer | What it is |
| --- | --- |
| Kernel | xnu-10063.141.1 (Darwin 23.6.0), 4K build, ~75-patch series: arch, SoC, OS profile, then this board's four patches in `patches/xnu/` |
| Build | UniXNU driver + Linux toolchain (upstream clang, mig, availability headers); prelink via the XnuPrelink kit |
| Firmware | Mu-Silicium fork (EDK II), `Platforms/Xiaomi/miroPkg`, BootShim; vendor binaries from the upstream `Device-Binaries` checkout |
| Storage | `drivers/MiroUfs`: SM8750 UFS host bring-up (fork of sheng's ShengUfs), read-only root publication, persistent Data store window on the `linux` GPT entry |
| Userland | Stock iPadOS 17.7.11 cryptex (SystemOS), owner-cache path instead of the AMFI-gated cryptex mount |
| Debug host | Mainline Linux 7.2.6 (piano tree) over `fastboot boot`, USB-NCM gadget, dropbear — see [UniXNU-miro-linux](https://github.com/Nanawwa/UniXNU-miro-linux) |

## Approach

- **Derive, don't rewrite.** The closest sibling port on the same SoC provides the shared layer; board work is identity, memory and storage integration.
- **RAM-only until storage is trusted.** Every test boots over `fastboot boot`. The only partition the loop writes is the reader's DTBO into the active slot. Any other write needs a backup and a rollback path first.
- **Diagnostics before features.** Each boot leaves a readable trace — the console ring on the `oops` partition, the panic text in the Android ramoops zone, netconsole before UFS is up. A failed boot costs one power press, not a photograph.
- **Userland before drivers.** Reach launchd with the stock cryptex first; display, touch and GPU come after the boot chain is trusted.
- **One identity, renamed once.** The board is `miro` everywhere: boot args (`miro-log`, `miro-autoreset`), on-disk magic (`MIROLOG2`, `MIROPNC1`), DTS compatible, payload volume `MIROESP`.

## Debugging

The unattended loop ([UniXNU-miro-linux](https://github.com/Nanawwa/UniXNU-miro-linux)) runs the whole cycle:

```
boot XNU image → crash or timed autoreset (miro-autoreset) → ABL lands in fastboot
  → reader Linux boots → reads the 16 MiB oops ring over SSH → parses it → reboots → next round
```

| Channel | Catches | Notes |
| --- | --- | --- |
| `oops` ring | Full console after UFS is up | 248 slots × 64 KiB, `OOPSLOG1` slots; panic record in the tail |
| pstore ramoops | Panic text, survives the warm reset | Read from the reader at `/sys/fs/pstore/console-ramoops-0`; the reader's own boot overwrites it, so read before the next round |
| netconsole | Pre-UFS early boot | UDP 6666 to the debug host; needs the kernel up but not storage |
| Screen | Firmware menu, panic screen | The one channel that needs eyes |

The full method write-up - how to tell which layer died, what the static checks cover, and the traps that cost real time here - is in [docs/DEBUGGING.md](docs/DEBUGGING.md).

Kernel arguments live in `board.toml` (`[payload] kernel_args`): `miro-log=1` opens the ring, `miro-data-persist=2` opens the Data store, `miro-no-cs=1` stands in for the StaticTrustCache during bring-up, `miro-dyld-verbose=1` injects `DYLD_PRINT_*` into launchd's environment, `miro-autoreset=<s>` returns to the bootloader after launchd starts.

## Layout

| Path | Contents |
| --- | --- |
| `board.toml` | Board configuration: components, archive plan, payload policy, ADT tables |
| `configs/miro_defconfig` | Kernel configuration |
| `dts/miro.dts` | Memory map, reservations, timer, ADT exports |
| `drivers/MiroUfs` | SM8750 UFS transport |
| `drivers/MiroPlatformPower` | PMGR instrument (sheng's, addressed for this kernelcache) |
| `package/` | UEFI-side components: boot log, boot progress, diagnostics |
| `patches/xnu/` | The board's four XNU patches (see `patches/xnu/series`) |
| `porting.toml` | Per-area status, evidence and next steps |
| `docs/` | Pinned values, UFS notes, handoff journal, main-repository patches |

## Build (Linux host)

```sh
git clone --recurse-submodules https://github.com/UniXNU/UniXNU
git clone --recurse-submodules https://github.com/Nanawwa/UniXNU-miro
git clone https://github.com/IssacL04/UniXNU-sm8750-common   # public SM8750 mirror

cd UniXNU
sudo scripts/linux/setup-toolchain.py
./unixnu defconfig miro_defconfig -D ../UniXNU-miro
./unixnu kernel -D ../UniXNU-miro -j$(nproc)
./unixnu payload -D ../UniXNU-miro --unique-chip-id <64-bit-hex> \
    --data-ramdisk <packed-data-seed>.dbr --trust-cache <static-trustcache>.bundle
./unixnu bootimg -D ../UniXNU-miro --fd <SILICIUM_UEFI.fd> --dtb ../uefi-miro/Resources/DTBs/miro.dtb
```

The SM8750 source resolves through `local.toml` (see `local.toml.example`) until the maintainer's repository is published; `feeds.toml` records the intended pin. The firmware volume comes from the Mu-Silicium fork carrying `miroPkg` (publication pending). The Data seed and trust cache are per-setup inputs, not part of the repository.

## License

Same license as the upstream UniXNU device repositories; see `LICENSE`. The UFS transport and PMGR instrument descend from the sheng and dada ports and keep their notices.
