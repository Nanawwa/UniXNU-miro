# MiroUfs

miro's UFS storage driver. Owns the SM8750 host controller bring-up, the SMMU takeover, the request
queue, GPT/window parsing and the read-only root publication.

This is the board's own driver, derived from the `ShengUfs` component (SM8550) and reduced to the
SM8750 facts:

- accepted controller names: `qcom,ufshc` and `qcom,sm8750-ufshc`; the SM8550 name is gone.
- GCC clock offsets: the SM8750 values only (`GccUfs.h`).
- root partition: the dedicated `linux` GPT entry (`kRootPartition`), not sheng's `userdata`.
- optional policy arguments use the `miro-` prefix (`miro-log`, `miro-data-persist`,
  `miro-log-reserve`); the board passes none of them and leaves both user-partition windows closed.

## Write windows

`RootCommands.h` allows at most three windows of 4096-byte LBAs on LUN 0. Each opens once, they refuse to overlap, and every
write is checked against them again, so nothing outside them can be written even if a caller is wrong.

| window | opened by | where | boot argument |
| --- | --- | --- | --- |
| Data store overlay | `hostStoreOpen` | the root partition's end | `sheng-data-persist=2` (mode 1 proves the window and reads only) |
| diagnostic log and export | `hostLogOpen` | the root partition's last 32 MiB and the 256 MiB below it | `miro-log=1` |
| panic record and bring-up probe | `hostPanicOpen` | the end of the board's crash-log partition (`oops` on this phone) | none: the panic screen calls it |

The miro board keeps the first two closed. Both sit in the user partition, and on 2026-10-29 a window opened at the end of
userdata destroyed live filesystem data with no way back. The panic record takes its target from `rootNamedRange`, which reads
only the GPT and has no root-partition fallback: if `oops` is absent or too small, the record is not written at all. Read it
back with `scripts/read-record-oops.sh` and `scripts/read-miro-log.py --oops`.

The source file and runtime classes keep the upstream names (`MiroUfs.cpp` is the fork point;
`NativeUfs`/`NativeUfsBlock` are the IOKit personalities) so the prelink and binding records stay
comparable with the component it came from. See `docs/ufs-miro.md`.

Linked into XNU by `unixnu build kernel` through `component:MiroUfs`.