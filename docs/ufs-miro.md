# MiroUfs on SM8750 (miro)

miro's UFS driver: what it needs from the board and the SM8750 SoC, and where each fact comes from.
The driver is a fork of the sheng (SM8550) `ShengUfs` component, reduced to the SM8750 facts.
The authoritative source for the clock side is the mainline Linux clock driver,
`miro_kernel/linux/drivers/clk/qcom/gcc-sm8750.c`; the device tree facts come from the stock
`uefi-miro/Resources/DTBs/miro.dtb`.

## Why this is a board component

The upstream `ShengUfs` component is the sheng UFS transport; its catalog entry points at
`https://github.com/UniXNU/ShengUfs.git`, but that source has no SM8750 controller facts and carries
the sheng root-partition and log policy. This board therefore owns its driver: `MiroUfs`, kept as a
plain directory `drivers/MiroUfs` instead of a submodule, so the SM8750 facts and the `linux` root
partition live with the board they were measured for. `unixnu status` reports it as a device module:

```
driver   MiroUfs                 device
```

The fork drops the SM8550 controller name and its annotated clock table, and it renames the optional
policy arguments to `miro-*`. The source file and runtime classes keep the upstream names
(`MiroUfs.cpp` is the fork point; `NativeUfs`/`NativeUfsBlock` are the IOKit personalities), as the
catalog records in `runtime_identifiers`. Publish the fork as its own repository before pinning it in
`feeds.toml`.

## 1. Controller compatible

The miro device tree names the controller:

```
ufshc@1d84000 {
    compatible = "qcom,ufshc";
```

The sheng port matched only `"qcom,sm8550-ufshc"`, which is not a substring of `"qcom,ufshc"`, so
`NativeUfs::probe` returned `nullptr` and the driver never attached. `MiroUfs` accepts the two SM8750
shapes and nothing else:

| Compatible | Tree |
| --- | --- |
| `qcom,ufshc` | miro (stock SM8750 tree) |
| `qcom,sm8750-ufshc` | SM8750 trees that follow `sm8750.dtsi` ordering |

The match stays a property read on the provider (`IOPlatformDevice`), so a device tree that publishes
no `compatible` still does not attach.

## 2. GCC UFS clock offsets

Eight of the nine branch offsets differ between SM8550 and SM8750. `GccUfs.h` now carries the SM8750
values and records the SM8550 one each replaces. Addresses from `clk_branch::halt_reg` in
`gcc-sm8750.c` (all use `halt_bit` 0, enable bit 0):

| Macro | SM8750 | SM8550 |
| --- | --- | --- |
| `GCC_UFS_PHY_AXI_ENABLE` | `0x77018` | `0x77018` |
| `GCC_UFS_PHY_AHB_ENABLE` | `0x77028` | `0x77024` |
| `GCC_UFS_PHY_TX_SYMBOL0_ENABLE` | `0x7702c` | `0x77028` |
| `GCC_UFS_PHY_RX_SYMBOL0_ENABLE` | `0x77030` | `0x7702c` |
| `GCC_UFS_PHY_UNIPRO_CORE_ENABLE` | `0x7706c` | `0x77068` |
| `GCC_UFS_PHY_ICE_CORE_ENABLE` | `0x7707c` | `0x77074` |
| `GCC_UFS_PHY_PHY_AUX_ENABLE` | `0x770bc` | `0x770b0` |
| `GCC_UFS_PHY_RX_SYMBOL1_ENABLE` | `0x770d8` | `0x770cc` |
| `GCC_AGGRE_UFS_PHY_AXI_ENABLE` | `0x770f0` | `0x770e4` |

The two GDSC registers are unchanged: `gcc_ufs_phy_gdsc` at `0x77004` and `gcc_ufs_mem_phy_gdsc` at
`0x9e000`.

Unchanged from sheng, and verified to still hold on SM8750:

- the physical windows: `GCC_BASE = 0x00100000`, `UFS_HC_BASE = 0x01d84000`, `UFS_HC_SIZE = 0x3000`
  (the stock tree has `clock-controller@100000` and `ufshc@1d84000` with `reg` size `0x3000`);
- every offset above still falls inside the `0x77000 + 0x1000` window that `inspect()` maps, the
  highest being `0x770f0` (240 bytes short of the end);
- `hwcg_bit = 1` on `axi`, `ahb`, `unipro_core`, `ice_core`, `phy_aux` and `aggre`, so `CLK_OFF` on an
  idle branch is normal and the enable bits, not `CLK_OFF`, gate the read-only probe.

The four RCG2 command registers (`0x77034`, `0x7708c`, `0x770a4`, `0x770c0`) are recorded in
`GccUfs.h` but not programmed: `NativeUfs` still performs no rate change.

## 3. Write safety

The GCC window remains read-only. All 13 `write32` calls in `NativeUfs.cpp` target either the SMMU
(`0x304`-style offsets inside the mapped SMMU window) or the UFS host controller at `UFS_HC_BASE`
(`0x20`, `0x24`, `0x50`, `0x54`, `0x58`, `0x60`, and the doorbell helper). A wrong clock offset can
therefore corrupt a *read*, which makes the probe report a misleading power state, but cannot corrupt
a GCC register.

## 4. What the mainline Linux port says

`miro_kernel/linux` is a working Linux on this phone, and its UFS stack is the reference for what a
real SM8750 UFS host needs. Three facts from it agree with this port:

- **The host controller programming is the SM8550 one.** `drivers/ufs/host/ufs-qcom.c` maps
  `qcom,sm8750-ufshc` to `ufs_qcom_sm8550_drvdata` (`quirks = UFSHCD_QUIRK_BROKEN_LSDBS_CAP`,
  `no_phy_retention = true`), the same data as `qcom,sm8550-ufshc`. So a transport written for
  SM8550 is the right starting point for SM8750; only the clock offsets differ, and they are in
  section 2.
- **The SMMU stream is 0x60.** The mainline `sm8750.dtsi` node `ufs_mem_hc` has
  `iommus = <&apps_smmu 0x60 0>`, which is the stream id the driver's own SMMU takeover looks for.
- **The clock branches are the eight the driver programs**: `core_clk` (GCC_UFS_PHY_AXI_CLK),
  `bus_aggr_clk` (GCC_AGGRE_UFS_PHY_AXI_CLK), `iface_clk` (GCC_UFS_PHY_AHB_CLK),
  `core_clk_unipro` (GCC_UFS_PHY_UNIPRO_CORE_CLK), `ref_clk` (RPMH_LN_BB_CLK3), `tx_lane0_sync_clk`,
  `rx_lane0_sync_clk` and `rx_lane1_sync_clk`. The `GccUfs.h` table has nine entries: those eight
  plus the ICE core clock, which this driver does not program.

The mainline stack also *programs* the PHY (`drivers/phy/qualcomm/phy-qcom-qmp-ufs.c`, QMP v4 for
sun). This driver does not: it takes over the controller the firmware left running. That is a
reasonable assumption on this phone, because the phone's own UEFI drives the same UFS before
DarwinBoot runs (`uefi-miro/Mu_Basecore/MdeModulePkg/Bus/Ufs/UfsBlockIoPei`, `UfsPassThruDxe`).

## 5. Block-device half: reference object and kext bindings

The transport derives from `IOBlockStorageDevice`, and that class is not in the xnu tree: the port
compiles it from Apple's IOStorageFamily source. The kernelcache's own
`com.apple.iokit.IOStorageFamily` kext defines the same class name, and `OSMetaClass` refuses a second
registration:

```
OSMetaClass: Kext com.apple.iokit.IOStorageFamily class IOBlockStorageDevice is a duplicate;
 kext __kernel__ already has a class by that name. (libkern/c++/OSMetaClass.cpp:894)
```

The prelinked kext cannot simply be dropped: `com.apple.driver.AppleARMPlatform` and
`com.apple.filesystems.apfs` both list it in `OSBundleLibraries`, so the prelink closure pulls it back
in.

The board therefore compiles the base class only as a reference and never links it:

- `scripts/build-storage-classes.py` compiles `IOBlockStorageDevice.cpp` from the pinned
  IOStorageFamily revision into `build/storage-classes/IOBlockStorageDevice.o`; the object supplies the
  190-slot dispatch shape the binding step reads.
- `scripts/build-storage-kext-bindings.py --component MiroUfs` reads that shape and the pinned
  kernelcache, finds the single 190-pointer vtable of the kext, and emits `.set` constants for the
  slots the transport imports. It rewrites `build/miro-ufs/libMiroUfs.a` with the bound object and
  the bindings assembly.
- The kernel script links the archive named by `CONFIG_XNU_NATIVE_UFS_ARCHIVE` (the board sets it to
  `build/miro-ufs/libMiroUfs.a`) with `-force_load`; the kernelcache's kext registers the class, and
  the transport calls its implementation. No duplicate class exists.

The driver's own `XNU_NATIVE_UFS_PROBE_ONLY` guard remains available for a probe-only build, but this
board does not use it: the payload needs the block device, and the binding path makes it safe.

The hardware work is unchanged: the ladder `native-ufs-probe=1..13` maps the GCC and the controller,
dumps their registers, takes over the controller the firmware left up and issues read-only SCSI
commands. The board additionally passes `native-ufs-root=1` and `native-ufs-diagnostics=1`.

The first level is the register dump. What it has to show:

- `gdsc` bit 31 set (the UFS GDSC is powered) and the `axi`/`ahb` branches enabled;
- `hc_read=1` with a plausible `cap` (`0x00010000`-ish, `HCS`/`HCE` capability bits) and `ver`;
- `utrlba` pointing at the firmware's request list.

## 6. Still open
- **UFS PHY**: the stock tree has `ufsphy_mem@1d80000` with `compatible = "qcom,ufs-phy-qmp-v4-sun"`,
  a sun-specific QMP PHY. `NativeUfs` does not program the PHY; it relies on the firmware having left
  the link up, as on sheng.
- **SMMU**: `apps_smmu` stream mapping for the UFS master is not represented. The sheng path takes
  over the SMMU by hand (`SmmuTakeover.h`); the SM8750 context bank numbers are not in the stock tree
  in a form this driver reads, so the takeover is unverified on miro.
- **ICE**: the inline crypto engine sits at `0x1d90000` per the stock tree (`reg-names = "ice"`).
  `NativeUfs` does not configure it; reads are unencrypted through the same path sheng uses.
- **Root publication**: the transport adopts the controller and reads the whole GPT on the phone, but
  no userdata image has been flashed. The root window, its verification policy and persistence remain
  unqualified.
