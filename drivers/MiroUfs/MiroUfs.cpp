// SPDX-License-Identifier: BSD-2-Clause-Patent
// UFS bring-up for the Redmi K80 Pro (miro, Qualcomm SM8750). This is the board's own transport, derived
// from the ShengUfs component and reduced to the SM8750 facts: one controller compatible list, the
// SM8750 GCC offsets, and the dedicated "linux" root partition. Default-off: no MMIO is touched unless
// native-ufs-probe=1 or native-ufs-root=1 is present.
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
// The block-device half of this driver derives from IOBlockStorageDevice, a class of Apple's separate
// IOStorageFamily project and not of the xnu tree. The base class has to be compiled into the kernel
// for the driver to link, and the kernelcache's own com.apple.iokit.IOStorageFamily kext defines the
// same class name: OSMetaClass refuses the second registration ("class IOBlockStorageDevice is a
// duplicate; kext __kernel__ already has a class by that name"). In probe mode the block half is left
// out entirely, so the driver links without the base class and the prelinked kext stays where it is.
#if !defined(XNU_NATIVE_UFS_PROBE_ONLY) || !(XNU_NATIVE_UFS_PROBE_ONLY)
#include <IOKit/storage/IOBlockStorageDevice.h>
#define NATIVE_UFS_BLOCK_DEVICE 1
#endif
#include <libkern/crypto/sha2.h>
#include <vm/vm_kern.h>
#include <vm/vm_map.h>
#include <sys/param.h>
#include <sys/vnode.h>
#include <sys/vfs_context.h>
#if defined(NATIVE_VERIFIED_BLOCK) && NATIVE_VERIFIED_BLOCK
#include "verified_root.h"
#include "VerifiedBlockPolicy.h"
extern "C" {
extern const struct vr_crypto vr_xnu_crypto;
extern const uint8_t verified_root_key[];
}
#endif
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSIterator.h>
#include <IOKit/IOBSD.h>
#include <kern/thread.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>
#include <kern/debug.h>
#include "GccUfs.h"
#include "RootDiagnostics.h"
#include "TransferPoll.h"
#include "SmmuTakeover.h"

static UfsRootDiagnostics gRootDiagnostic;
static bool gRootDiagnosticEnabled;
static void rootDiagnosticFailure (const char *reason);

extern "C" {
// Optional passive observer; absent in ordinary builds. It may run while a
// window lock is held, so its contract forbids blocking or transport access.
void boot_progress_poll (void) __attribute__((weak));
volatile uint32_t native_ufs_probe_calls = 0;
volatile uint32_t native_ufs_matched = 0;
volatile uint32_t native_ufs_probe_requested = 0;
static unsigned gLastPolls = 0;
static uint32_t gLastRb0 = 0, gLastRb8 = 0;
volatile uint32_t native_ufs_mapped = 0;
volatile uint32_t native_ufs_status = 0;
// Root start: 1 entered, 2 host up, 3 layout found; the last soft-failure reason (a static string), for the boot page.
volatile uint32_t native_ufs_root_step = 0;
const char *volatile native_ufs_root_failure = nullptr;
volatile uint32_t native_ufs_cap = 0;
volatile uint32_t native_ufs_version = 0;
volatile uint32_t native_ufs_hcs = 0;
volatile uint32_t native_ufs_hce = 0;
volatile uint32_t native_ufs_utrl_ready = 0;
volatile uint32_t native_ufs_uic_status = 0;
volatile uint32_t native_ufs_uic_result = 0;
}

class NativeUfs : public IOService {
  OSDeclareDefaultStructors (NativeUfs)
  OSSharedPtr<IOMemoryMap> fMap;
  OSSharedPtr<IOBufferMemoryDescriptor> fCmdDesc;
  void inspect (void);
  void adoptAndRead (unsigned level);
  void dumpFirmwareList (void);
  void inspectSmmu (void);
  void inspectContextBanks (void);
  bool startRoot (IOService *provider);
public:
  static bool waitSet (uintptr_t base, unsigned offset, uint32_t mask, unsigned loops);
  IOService *probe (IOService *provider, SInt32 *score) override;
  bool start (IOService *provider) override;
  void stop (IOService *provider) override;
};

OSDefineMetaClassAndStructors (NativeUfs, IOService)

static uint32_t read32 (uintptr_t base, unsigned offset)
{
  return *reinterpret_cast<volatile uint32_t *> (base + offset);
}

static void write32 (uintptr_t base, unsigned offset, uint32_t value)
{
  *reinterpret_cast<volatile uint32_t *> (base + offset) = value;
}

// A device tree "compatible" property is a list of NUL-terminated strings, and IODTPlatformExpert
// copies it into the nub unchanged, so the provider's property is an OSData of that list. Compare
// whole entries: matching inside an entry would also accept a name that merely starts with one of
// them ("qcom,ufshc2"), and that has to stay a non-match.
static bool listContains (const char *data, size_t length, const char *wanted)
{
  size_t lengthWanted = 0;
  while (wanted[lengthWanted]) {
    lengthWanted++;
  }
  size_t at = 0;
  while (at < length) {
    size_t end = at;
    while (end < length && data[end] != '\0') {
      end++;
    }
    if (end - at == lengthWanted && memcmp (data + at, wanted, lengthWanted) == 0) {
      return true;
    }
    at = end + 1;
  }
  return false;
}

IOService *NativeUfs::probe (IOService *provider, SInt32 *score)
{
  native_ufs_probe_calls = native_ufs_probe_calls + 1;
  if (OSDynamicCast (IOPlatformExpertDevice, provider)) {
    // Second personality (KernelConfigTables: IOMatchCategory NativeUfsRoot): attaches to the
    // root device, which starts before the platform expert publishes IOResources, so the
    // hardware probes run before the original AMFI (whose start panics without a PMGR).
    native_ufs_matched = 1;
    if (score) {
      *score = 500;
    }
    return this;
  }
  OSObject *property = provider->getProperty ("compatible");
  OSData *data = OSDynamicCast (OSData, property);
  OSString *string = OSDynamicCast (OSString, property);
  // The stock miro tree names the controller "qcom,ufshc" alone; the mainline sm8750.dtsi lists the
  // SoC-qualified name first. Accept both: this is an SM8750 board and a later tree may publish
  // either shape. The compare is per entry, not a substring search (see listContains).
  static const char *const accepted[] = { "qcom,ufshc", "qcom,sm8750-ufshc" };
  bool match = false;
  if (data) {
    const char *text = static_cast<const char *> (data->getBytesNoCopy ());
    for (size_t index = 0; text && index < sizeof (accepted) / sizeof (accepted[0]); index++) {
      match = match || listContains (text, data->getLength (), accepted[index]);
    }
  } else if (string) {
    // An OSString property is one name; listContains also handles that shape, because a buffer with no
    // trailing NUL is compared as a single final entry.
    const char *text = string->getCStringNoCopy ();
    for (size_t index = 0; text && index < sizeof (accepted) / sizeof (accepted[0]); index++) {
      match = match || listContains (text, string->getLength (), accepted[index]);
    }
  }
  if (!match) {
    return nullptr;
  }
  native_ufs_matched = 1;
  if (score) {
    *score = 1000;
  }
  return this;
}

bool NativeUfs::waitSet (uintptr_t base, unsigned offset, uint32_t mask, unsigned loops)
{
  for (unsigned index = 0; index < loops; index++) {
    if ((read32 (base, offset) & mask) == mask) {
      return true;
    }
    IODelay (1);
  }
  return false;
}

// Read-only inspection (native-ufs-probe=2): reports what firmware left behind.
// The physical windows are the stock device tree's GCC (0x100000) and UFS host
// (0x1d84000, 0x3000) ranges. The controller is read only if the GCC says its
// GDSC is powered and the AXI/AHB branches are running; nothing is written.
static uintptr_t mapPhys (uint64_t phys, uint64_t length, OSSharedPtr<IOMemoryMap> &keep)
{
  IODeviceMemory *memory = IODeviceMemory::withRange (phys, length);
  if (!memory) {
    return 0;
  }
  keep = OSSharedPtr<IOMemoryMap> (memory->map (kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain);
  memory->release ();
  return keep ? static_cast<uintptr_t> (keep->getVirtualAddress ()) : 0;
}

void NativeUfs::inspect (void)
{
  OSSharedPtr<IOMemoryMap> gccMap, memPhyMap, hcMap;
  uintptr_t gcc = mapPhys (GCC_BASE + 0x77000, 0x1000, gccMap);
  uintptr_t memPhy = mapPhys (GCC_BASE + 0x9e000, 0x1000, memPhyMap);
  if (!gcc || !memPhy) {
    panic ("NATIVE_UFS inspect: GCC map failed gcc=%p memphy=%p", (void *) gcc, (void *) memPhy);
  }
  uint32_t gdsc = read32 (gcc, GCC_UFS_PHY_GDSCR - 0x77000);
  uint32_t axi = read32 (gcc, GCC_UFS_PHY_AXI_ENABLE - 0x77000);
  uint32_t ahb = read32 (gcc, GCC_UFS_PHY_AHB_ENABLE - 0x77000);
  uint32_t unipro = read32 (gcc, GCC_UFS_PHY_UNIPRO_CORE_ENABLE - 0x77000);
  uint32_t txs = read32 (gcc, GCC_UFS_PHY_TX_SYMBOL0_ENABLE - 0x77000);
  uint32_t rxs = read32 (gcc, GCC_UFS_PHY_RX_SYMBOL0_ENABLE - 0x77000);
  uint32_t aux = read32 (gcc, GCC_UFS_PHY_PHY_AUX_ENABLE - 0x77000);
  uint32_t memGdsc = read32 (memPhy, 0);
  // GDSCR bit31 PWR_ON; CBCR bit31 CLK_OFF.
  bool powered = (gdsc & 0x80000000U) != 0;
  // Hardware 2026-10-17: AXI running, AHB enabled but CLK_OFF (bit31). With bit1 (HW gating)
  // set the branch wakes on access, so enable bits, not CLK_OFF, gate the read-only probe.
  bool busClocks = !(axi & 0x80000000U) && (ahb & 0x1U) && (axi & 0x1U);
  uint32_t cap = 0, ver = 0, hcs = 0, hce = 0, utrlba = 0, is = 0;
  unsigned readHc = 0;
  if (powered && busClocks) {
    uintptr_t hc = mapPhys (UFS_HC_BASE, UFS_HC_SIZE, hcMap);
    if (hc) {
      cap = read32 (hc, 0x00);
      ver = read32 (hc, 0x08);
      is = read32 (hc, 0x20);
      hcs = read32 (hc, 0x30);
      hce = read32 (hc, 0x34);
      utrlba = read32 (hc, 0x50);
      readHc = 1;
    }
  }
  panic ("NATIVE_UFS inspect\n"
         "gdsc=%08x memphygdsc=%08x\n"
         "axi=%08x ahb=%08x unipro=%08x\n"
         "txsym=%08x rxsym=%08x aux=%08x\n"
         "hc_read=%u cap=%08x ver=%08x\n"
         "is=%08x hcs=%08x hce=%08x utrlba=%08x",
         gdsc, memGdsc, axi, ahb, unipro, txs, rxs, aux, readHc, cap, ver, is, hcs, hce, utrlba);
}

// Adopt the controller the firmware left running (native-ufs-probe=3) and issue
// read-only SCSI commands through one UTP transfer request slot. Nothing is written
// to the storage. The controller is not reset and the link is not restarted; the
// firmware's request list is stopped (refused if a request is outstanding) and replaced
// by ours. Interrupts stay masked and completion is polled.
namespace {
struct UfsResult {
  uint32_t ocs;      // UTRD overall command status; 0xf = never completed
  uint32_t response; // UPIU response byte
  uint32_t scsi;     // SCSI status byte
  uint32_t sense;    // sense key<<16 | ASC<<8 | ASCQ from the response UPIU's autosense data
  uint32_t senseLen; // sense data length reported by the device
  uint32_t seg;      // UPIU data segment length
};

static void be32 (uint8_t *at, uint32_t value)
{
  at[0] = value >> 24; at[1] = value >> 16; at[2] = value >> 8; at[3] = value;
}

static uint32_t getBe32 (const uint8_t *at)
{
  return (uint32_t (at[0]) << 24) | (uint32_t (at[1]) << 16) | (uint32_t (at[2]) << 8) | at[3];
}
}

static void cacheClean (const void *start, size_t length)
{
  uintptr_t address = reinterpret_cast<uintptr_t> (start) & ~uintptr_t (63);
  uintptr_t end = reinterpret_cast<uintptr_t> (start) + length;
  for (; address < end; address += 64) {
    __asm__ volatile ("dc cvac, %0" :: "r" (address) : "memory");
  }
  __asm__ volatile ("dsb sy" ::: "memory");
}

static void cacheCleanInvalidate (const void *start, size_t length)
{
  uintptr_t address = reinterpret_cast<uintptr_t> (start) & ~uintptr_t (63);
  uintptr_t end = reinterpret_cast<uintptr_t> (start) + length;
  for (; address < end; address += 64) {
    __asm__ volatile ("dc civac, %0" :: "r" (address) : "memory");
  }
  __asm__ volatile ("dsb sy" ::: "memory");
}

// The region is mapped as device memory: only aligned element-sized stores are allowed
// (bzero's block zeroing faulted on hardware, 2026-10-17).
static void zero32 (void *start, size_t length)
{
  volatile uint32_t *word = static_cast<volatile uint32_t *> (start);
  for (size_t index = 0; index < length / 4; index++) {
    word[index] = 0;
  }
}


// Give the UFS stream a pass-through mapping the way Linux does on this firmware.
// Hardware 2026-10-17: after hand-off apps_smmu has sCR0 = 0x002f0406 (USFCFG=1, unmatched
// streams fault), no stream-match entry for the UFS host (ID 0x60, `iommus = <&apps_smmu 0x60
// 0x0>` in the board DT), a TYPE=BYPASS S2CR reads back as FAULT, and sCR0 writes are ignored.
// Linux (drivers/iommu/arm/arm-smmu/arm-smmu-qcom.c, qcom_smmu_cfg_probe + qcom_smmu_write_s2cr,
// "bypass_quirk") meets exactly this: it reserves the LAST context bank (num_context_banks-1),
// writes SCTLR=0 (stage 1 disabled) and CBAR.TYPE=S1_TRANS_S2_BYPASS there, and maps a bypass
// stream as S2CR TYPE=TRANSLATE with CBNDX = that bank. S2CR is written before SMR
// (arm_smmu_write_sme). ARM IHI 0062D.c table 3-1 additionally requires VA64=1
// for inputs above 32 bits even with SCTLR.M=0. Storage DMA path only; this
// is physical pass-through, not a page-table or Adreno 4 KiB qualification.
static void smmuBypassStream (uint16_t stream, OSSharedPtr<IOMemoryMap> &keep, uint32_t *report,
                              uint64_t dma, uint64_t bytes)
{
  uintptr_t smmu = mapPhys (0x15000000ULL, 0xd0000, keep);
  if (!smmu) {
    panic ("NATIVE_UFS smmu: map failed");
  }
  UfsSmmuPlan plan = planUfsSmmu (stream, dma, bytes, [smmu](unsigned offset) { return read32 (smmu, offset); });
  gRootDiagnostic.smmuId1 = read32 (smmu, 0x24);
  gRootDiagnostic.smmuId2 = read32 (smmu, 0x28);
  gRootDiagnostic.smmuSlot = plan.slot;
  if (plan.error) {
    gRootDiagnostic.stage = "SMMU planning";
    gRootDiagnostic.result = static_cast<uint32_t> (kIOReturnNotReady);
    gRootDiagnostic.smr = plan.smr; gRootDiagnostic.s2cr = plan.s2cr;
    rootDiagnosticFailure (plan.error);
    panic ("NATIVE_UFS smmu: %s slot=%u smr=%x s2cr=%x", plan.error, plan.slot, plan.smr, plan.s2cr);
  }
  unsigned bank = plan.bank, index = plan.slot;
  uintptr_t cb = smmu + (plan.pages + bank) * 4096;
  const char *failure = configureUfsSmmu (plan, stream,
    [smmu](unsigned offset) { return read32 (smmu, offset); },
    [smmu](unsigned offset, uint32_t value) {
      write32 (smmu, offset, value);
      __asm__ volatile ("dsb sy" ::: "memory");
    });
  uint32_t smr = read32 (smmu, 0x800 + 4 * index), s2cr = read32 (smmu, 0xc00 + 4 * index);
  uint32_t cbar = read32 (smmu, 0x1000 + 4 * bank), sctlr = read32 (cb, 0x0);
  gRootDiagnostic.smr = smr; gRootDiagnostic.s2cr = s2cr;
  gRootDiagnostic.cbar = cbar; gRootDiagnostic.sctlr = sctlr;
  gRootDiagnostic.smmuId2 = read32 (smmu, 0x28);
  gRootDiagnostic.cba2r = read32 (smmu, 0x1800 + 4 * bank);
  if (failure) {
    gRootDiagnostic.stage = "SMMU readback";
    gRootDiagnostic.result = static_cast<uint32_t> (kIOReturnNotReady);
    rootDiagnosticFailure (failure);
    panic ("NATIVE_UFS smmu: %s idx=%u bank=%u cba2r=%x", failure, index, bank,
           gRootDiagnostic.cba2r);
  }
  report[0] = index | (bank << 16);
  report[1] = cbar;
  report[2] = sctlr;
}

void NativeUfs::adoptAndRead (unsigned level)
{
  OSSharedPtr<IOMemoryMap> hcMap;
  uintptr_t hc = mapPhys (UFS_HC_BASE, UFS_HC_SIZE, hcMap);
  if (!hc) {
    panic ("NATIVE_UFS adopt: controller map failed");
  }
  // The controller's own request list is the one region its DMA is known to reach
  // (hardware 2026-10-17: a kernel-allocated buffer at 0xf080f000 reset the system when the
  // first command was issued, and nothing below 0xc8000000 is allocatable). The firmware
  // zeroed it; use 16 KiB from UTRLBA as ordinary cached memory (the DT marks the host
  // dma-coherent). Test instrument only: the kernel may also own these pages.
  uint64_t fwList = (uint64_t (read32 (hc, 0x54)) << 32) | read32 (hc, 0x50);
  if (!fwList || (fwList & 0xfff)) {
    panic ("NATIVE_UFS adopt: no usable firmware list base=%llx", fwList);
  }
  IOMemoryDescriptor *region = IOMemoryDescriptor::withPhysicalAddress (fwList, 16384, kIODirectionInOut);
  OSSharedPtr<IOMemoryMap> regionMap;
  if (region) {
    regionMap = OSSharedPtr<IOMemoryMap> (region->map (kIOMapAnywhere), OSNoRetain);
    region->release ();
  }
  if (!regionMap) {
    panic ("NATIVE_UFS adopt: firmware list map failed base=%llx", fwList);
  }
  uint8_t *virt = reinterpret_cast<uint8_t *> (regionMap->getVirtualAddress ());
  uint64_t phys = fwList;
  zero32 (virt, 16384);
  cacheClean (virt, 16384);
  // Layout: [0,4096) request list (slot 0 used), [4096,...) command + response UPIU +
  // PRDT, [8192,16384) data. Offsets follow UfsPassThruHci.c exactly: the controller
  // computes them as dword offsets from the UCD base (RuO = 32/4, RuL = 56/4,
  // PrdtO = (32+56)/4), so a larger reservation is not interchangeable with them.
  const uint32_t ucdOffset = 4096, rspOffset = 32, prdtOffset = 88, dataOffset = 8192;
  uint32_t hcsBefore = read32 (hc, 0x30);
  uint32_t doorbellBefore = read32 (hc, 0x58);
  if (doorbellBefore != 0 || read32 (hc, 0x78) != 0) {
    panic ("NATIVE_UFS adopt: firmware request outstanding doorbell=%08x hcs=%08x", doorbellBefore, hcsBefore);
  }
  OSSharedPtr<IOMemoryMap> smmuMap;
  uint32_t smmuReport[3] = { 0, 0, 0 };
  write32 (hc, 0x24, 0);            // mask interrupts
  // The firmware hands the controller over already running: HCE=1, UTRLRSR=1,
  // UTRLBA=d4bf7000, doorbell clear (probe=6). A stop/reprogram/start cycle does
  // not make it service a request afterwards (probe=4: doorbell stuck at 1, OCS
  // 0xf, empty response UPIU) and the SMMU records no fault for it (gfsr=0; the
  // stale cbfsr/FAR belong to an earlier firmware stream). Leave the run state
  // alone and publish the descriptor into the list the controller already owns.
  // HCE must stay set: dropping and raising it leaves HCS=0x8 (UCRDY only).
  smmuBypassStream (0x60, smmuMap, smmuReport, phys, 16384);
  write32 (hc, 0x20, 0xffffffffU);  // clear stale status
  if (!(read32 (hc, 0x60) & 1)) {
    write32 (hc, 0x60, 1);          // the firmware left it stopped; start it
    if (!NativeUfs::waitSet (hc, 0x30, 0x2, 100000)) {
      panic ("NATIVE_UFS adopt: request list not ready hcs=%08x hce=%08x ba=%08x",
             read32 (hc, 0x30), read32 (hc, 0x34), read32 (hc, 0x50));
    }
  }
  if (level == 3) {
    panic ("NATIVE_UFS adopt: list replaced\n"
           "phys=%llx hcs=%08x->%08x\n"
           "is=%08x db=%08x rsr=%08x ba=%08x bau=%08x ie=%08x",
           phys, hcsBefore, read32 (hc, 0x30), read32 (hc, 0x20), read32 (hc, 0x58),
           read32 (hc, 0x60), read32 (hc, 0x50), read32 (hc, 0x54), read32 (hc, 0x24));
  }

  auto command = [&] (uint8_t lun, const uint8_t *cdb, unsigned cdbLength, bool toHost, uint32_t length) -> UfsResult {
    uint8_t *utrd = virt;
    uint8_t *ucd = virt + ucdOffset;
    zero32 (utrd, 32);
    zero32 (ucd, 1024 + 16);
    zero32 (virt + dataOffset, 8192);
    ucd[0] = 0x01;                  // COMMAND UPIU
    ucd[1] = toHost ? 0x40 : 0x00;  // flags: read
    ucd[2] = lun;
    ucd[3] = 0;                     // task tag
    be32 (ucd + 12, length);        // expected data transfer length
    for (unsigned index = 0; index < cdbLength; index++) {
      ucd[16 + index] = cdb[index];
    }
    uint64_t ucdPhys = phys + ucdOffset;
    uint32_t direction = length ? (toHost ? 2U : 1U) : 0U;
    *reinterpret_cast<uint32_t *> (utrd + 0) = 0x11000000U | (direction << 25);
    *reinterpret_cast<uint32_t *> (utrd + 8) = 0x0fU;     // OCS: not completed
    *reinterpret_cast<uint32_t *> (utrd + 16) = static_cast<uint32_t> (ucdPhys & 0xffffff80U);
    *reinterpret_cast<uint32_t *> (utrd + 20) = static_cast<uint32_t> (ucdPhys >> 32);
    *reinterpret_cast<uint32_t *> (utrd + 24) = 14U | (8U << 16);   // RuL | RuO (dwords)
    uint32_t prdEntries = length ? 1U : 0U;
    *reinterpret_cast<uint32_t *> (utrd + 28) = prdEntries | (22U << 16);   // PrdtL | PrdtO
    if (length) {
      uint64_t dataPhys = phys + dataOffset;
      uint8_t *prd = ucd + prdtOffset;
      *reinterpret_cast<uint32_t *> (prd + 0) = static_cast<uint32_t> (dataPhys);
      *reinterpret_cast<uint32_t *> (prd + 4) = static_cast<uint32_t> (dataPhys >> 32);
      *reinterpret_cast<uint32_t *> (prd + 12) = length - 1;
    }
    cacheClean (virt, 16384);
    write32 (hc, 0x20, 0xffffffffU);
    // EDK2 UfsStartExecCmd: make sure the run-stop bit is set before the
    // doorbell. A stale doorbell bit from a stopped list keeps the slot
    // occupied, so clear it through UTRLCLR first (UFS HCI 5.4.4).
    if (!(read32 (hc, 0x60) & 1)) write32 (hc, 0x60, 1);
    // UTRLCLR (5.4.4): writing 1 to bit n clears doorbell bit n.
    if (read32 (hc, 0x58) & 1) write32 (hc, 0x5c, 1);
    write32 (hc, 0x58, 1);          // ring slot 0
    // Read the descriptor back through a fresh mapping: if the controller saw a
    // different value than we wrote, the write never reached the physical page
    // the controller reads (cache alias) rather than the controller rejecting it.
    __asm__ volatile ("dsb sy" ::: "memory");
    uint32_t rb0 = *reinterpret_cast<volatile uint32_t *> (virt + 0);
    uint32_t rb8 = *reinterpret_cast<volatile uint32_t *> (virt + 8);
    gLastRb0 = rb0; gLastRb8 = rb8;
    UfsResult result = { 0xf, 0xff, 0xff, 0, 0, 0 };
    bool done = false;
    unsigned polls = 0;
    // Original window: 100000 iterations of a 1 us delay. Restored after the
    // wide window proved the device never answers at all, so waiting longer
    // only delays the failure page.
    for (polls = 0; polls < 100000; polls++) {
      if (!(read32 (hc, 0x58) & 1)) {
        done = true;
        break;
      }
      IODelay (1);
    }
    gLastPolls = polls;
    cacheCleanInvalidate (virt, 16384);
    if (!done) {
      return result;
    }
    result.ocs = *reinterpret_cast<volatile uint32_t *> (utrd + 8) & 0xff;
    const uint8_t *rsp = ucd + rspOffset;
    result.response = rsp[6];
    result.scsi = rsp[7];
    // Response UPIU: 12-byte header, residual count, 16 reserved bytes, then the data segment
    // (2-byte sense length + sense data) at offset 32.
    result.seg = (uint32_t (rsp[10]) << 8) | rsp[11];
    result.senseLen = (uint32_t (rsp[32]) << 8) | rsp[33];
    result.sense = ((rsp[36] & 0xfU) << 16) | (uint32_t (rsp[46]) << 8) | rsp[47];
    return result;
  };

  // UIC commands are the one path this driver never exercised. HCS bits[10:8]
  // hold UPMCRS, and the value here is 1, which is the "link is in Hibernate"
  // state: the controller latches the doorbell and never completes, with IS
  // zero and no UIC error, because the UPIU cannot leave the host. DME_GET
  // reads a MIB attribute and DME_HIBERNATE_EXIT (0x18) brings the link back.
  auto uic = [&] (uint8_t opcode, uint16_t mib, uint32_t arg3, uint32_t *out) -> uint32_t {
    write32 (hc, 0x20, 0xffffffffU);          // clear UCCS and any stale status
    if (!NativeUfs::waitSet (hc, 0x30, 0x8, 100000)) {   // HCS.UCRDY must be set before UICCMD
      return 0xfffffffe;
    }
    write32 (hc, 0x94, (uint32_t (mib) << 16));
    write32 (hc, 0x98, 0);
    write32 (hc, 0x9c, arg3);
    write32 (hc, 0x90, opcode);
    if (!NativeUfs::waitSet (hc, 0x20, 0x400, 200000)) { // IS.UCCS
      return 0xfffffffd;
    }
    uint32_t arg2 = read32 (hc, 0x98);
    if (out) *out = read32 (hc, 0x9c);
    return arg2 & 0xff;                       // 0 is success
  };
  // The controller does not start servicing the UTP transfer list until it has
  // completed at least one UIC command after taking over from the firmware.
  // Probe 9 proved it: with no UIC traffic the doorbell sticks at 1 forever and
  // IS stays zero, while the same TEST UNIT READY completed in three polls
  // right after a DME_GET + DME_HIBERNATE_EXIT sequence -- and PA_PWR_MODE read
  // 0x11 (FAST_MODE), so the link was never hibernating. The wake-up was not
  // what helped; touching the UIC path was. Prime it before the first request.
  uint32_t uicPwr = 0, uicPwrStatus = 0, uicLanes = 0, uicLanesStatus = 0, uicExit = 0;
  uicPwrStatus = uic (0x01, 0x1571, 0, &uicPwr);       // DME_GET PA_PWR_MODE
  uicLanesStatus = uic (0x01, 0x1560, 0, &uicLanes);   // DME_GET PA_ActiveTxDataLanes
  uicExit = uic (0x18, 0, 0, nullptr);                 // DME_HIBERNATE_EXIT
  // NOP OUT (kind 0) and Query read requests (kind 1: opcode 0x05 READ FLAG, 0x03 READ ATTRIBUTE).
  // Read-only: no data segment, no state change on the device. Returns the UPIU response byte and
  // the 32-bit value field (attributes) or flag byte via *value; ocs 0xf means no completion.
  auto query = [&] (unsigned kind, uint8_t opcode, uint8_t idn, uint32_t *value, uint32_t *respByte) -> uint32_t {
    uint8_t *utrd = virt;
    uint8_t *ucd = virt + ucdOffset;
    zero32 (utrd, 32);
    zero32 (ucd, 1024 + 16);
    if (kind == 0) {
      ucd[0] = 0x00;                // NOP OUT
    } else {
      ucd[0] = 0x16;                // QUERY REQUEST
      ucd[5] = 0x01;                // query function (header dword 1, byte 1): standard read request
      ucd[12] = opcode;
      ucd[13] = idn;
    }
    ucd[3] = 1;                     // task tag
    uint64_t ucdPhys = phys + ucdOffset;
    *reinterpret_cast<uint32_t *> (utrd + 0) = 0x11000000U;
    *reinterpret_cast<uint32_t *> (utrd + 8) = 0x0fU;
    *reinterpret_cast<uint32_t *> (utrd + 16) = static_cast<uint32_t> (ucdPhys & 0xffffff80U);
    *reinterpret_cast<uint32_t *> (utrd + 20) = static_cast<uint32_t> (ucdPhys >> 32);
    *reinterpret_cast<uint32_t *> (utrd + 24) = 14U | (8U << 16);
    *reinterpret_cast<uint32_t *> (utrd + 28) = 22U << 16;
    cacheClean (virt, 16384);
    write32 (hc, 0x20, 0xffffffffU);
    // UTRIACR (5.4.7) can hold a non-zero aggregation timeout the firmware left:
    // with aggregation enabled the controller may hold completions, and a stale
    // value keeps the doorbell unserviced. Clear it for polled operation.
    write32 (hc, 0x4c, 0);
    if (!(read32 (hc, 0x60) & 1)) write32 (hc, 0x60, 1);
    if (read32 (hc, 0x58) & 1) write32 (hc, 0x5c, 1);
    write32 (hc, 0x58, 1);
    bool done = false;
    for (unsigned loops = 0; loops < 100000; loops++) {
      if (!(read32 (hc, 0x58) & 1)) {
        done = true;
        break;
      }
      IODelay (1);
    }
    cacheCleanInvalidate (virt, 16384);
    if (!done) {
      return 0xf;
    }
    const uint8_t *rsp = ucd + rspOffset;
    *respByte = (uint32_t (rsp[0]) << 8) | rsp[6];     // response type << 8 | query/NOP response
    *value = getBe32 (rsp + 20);
    return *reinterpret_cast<volatile uint32_t *> (utrd + 8) & 0xff;
  };
  // Device state before waking it. Hardware 2026-10-17: NOP OUT answers, fDeviceInit = 0 (done),
  // bCurrentPowerMode = 0x33 (UFS-PowerDown) and every LU returns NOT READY. Linux resumes the
  // device with START STOP UNIT (power condition ACTIVE) to the UFS Device well-known LU 0xD0
  // (ufshcd_set_dev_pwr_mode); no user data is touched.
  uint32_t powerBefore = 0, powerAfter = 0, queryResp = 0, ignoredValue = 0;
  uint32_t wakeStatus = 0xee;
  if (level == 5) {
    // The UIC prime already ran above. Report what it found and whether the
    // first storage command now completes, which is the whole question.
    uint8_t tur5[6] = { 0, 0, 0, 0, 0, 0 };
    UfsResult t5 = command (0, tur5, 6, false, 0);
    panic ("NATIVE_UFS probe5: UIC prime\n"
           "pwr=%08x st=%08x lanes=%08x st=%08x exit=%08x\n"
           "hcs=%08x is=%08x db=%08x rsr=%08x polls=%u\n"
           "ocs=%x resp=%02x scsi=%02x rsp=%08x %08x",
           uicPwr, uicPwrStatus, uicLanes, uicLanesStatus, uicExit,
           read32 (hc, 0x30), read32 (hc, 0x20), read32 (hc, 0x58), read32 (hc, 0x60), gLastPolls,
           t5.ocs, t5.response, t5.scsi,
           getBe32 (virt + ucdOffset + rspOffset), getBe32 (virt + ucdOffset + rspOffset + 4));
  }
  if (level == 9) {
    query (1, 0x03, 0x02, &powerBefore, &queryResp);     // READ ATTRIBUTE bCurrentPowerMode
    uint8_t ssu[6] = { 0x1b, 0, 0, 0, 0x10, 0 };          // START STOP UNIT, power condition ACTIVE
    UfsResult w = command (0xd0, ssu, 6, false, 0);
    wakeStatus = (w.ocs << 16) | (w.response << 8) | w.scsi;
    for (unsigned settle = 0; settle < 50; settle++) {
      IODelay (1000);
    }
    query (1, 0x03, 0x02, &powerAfter, &queryResp);
    (void) ignoredValue;
  }
  if (level == 4) {
    // REG_UFS_MEM_CFG (0x300) bit0 is MCQ_MODE_SELECT. Probe 8 measured it as
    // zero, so the firmware left this controller on the legacy UTRL path and
    // MCQ is not the reason the doorbell sticks. Report it so a future firmware
    // that flips the mode is visible immediately, and read MCQCAP at 0x04 for
    // the queue count and config block offset.
    uint32_t memCfg = read32 (hc, 0x300);
    uint32_t mcqCap = read32 (hc, 0x04);
    uint8_t tur0[6] = { 0, 0, 0, 0, 0, 0 };
    UfsResult t = command (0, tur0, 6, false, 0);
    const uint8_t *rsp0 = virt + ucdOffset + rspOffset;
    uint32_t dw0 = *reinterpret_cast<volatile uint32_t *> (virt + 0);
    // The SMMU is the other half of this failure: if the controller read the
    // request list but its DMA was rejected, the global fault register (0x48)
    // and the per-bank FSR/FAR at context bank + 0x58/0x60 say so, while the
    // doorbell stays set. Read them after the attempt.
    uintptr_t smmu = smmuMap ? static_cast<uintptr_t> (smmuMap->getVirtualAddress ()) : 0;
    uint32_t gfsr = 0, cbfsr = 0, farLo = 0, farHi = 0, cbBar = 0, cbSctlr = 0;
    if (smmu) {
      unsigned cbBank = (smmuReport[0] >> 16) & 0xffff;
      unsigned cbPages = 1U << (((read32 (smmu, 0x24) >> 28) & 7) + 1);
      uintptr_t cb = smmu + (cbPages + cbBank) * 4096;
      gfsr = read32 (smmu, 0x48);
      cbfsr = read32 (cb, 0x58);
      farLo = read32 (cb, 0x60);
      farHi = read32 (cb, 0x64);
      cbBar = read32 (smmu, 0x1000 + 4 * cbBank);
      cbSctlr = read32 (cb, 0x0);
    }
    // The real MCQ switch is REG_UFS_MEM_CFG (0x300) bit0, not 0x10: the values
    // dumped in probe 7 at 0x0c/0x10/0x14 were EXT_CONTROLLER_CAPABILITIES and
    // the controller PID/MID, which say nothing about the submission mode.
    // Probe 8 measured the bit as zero, so the legacy path is the live one.
    static char mcq[900];
    int used = snprintf (mcq, sizeof (mcq),
                         "memcfg=%08x sel=%u mcqcap=%08x qcfg=%08x maxq=%u\n",
                         memCfg, memCfg & 1, mcqCap,
                         ((mcqCap >> 16) & 0xff) * 0x200, (mcqCap & 0xff) + 1);
    // The queue config block sits at QCFGPTR*0x200 (0x1C000 on this part), well
    // outside the 0x3000 window we map, so record what the low window shows.
    for (unsigned off = 0x300; off < 0x320 && used < 800; off += 4) {
      used += snprintf (mcq + used, sizeof (mcq) - used, "%03x=%08x ", off, read32 (hc, off));
    }
    panic ("NATIVE_UFS adopt: TEST UNIT READY lun0\n"
           "ocs=%x resp=%02x scsi=%02x\n"
           "phys=%llx hcs=%08x->%08x smmu idx/bank=%08x cbar=%08x\n"
           "is=%08x db=%08x rsr=%08x hce=%08x iacr=%08x polls=%u\n"
           "utrd0=%08x ucd=%08x rb=%08x/%08x\n"
           "smmu gfsr=%08x cbfsr=%08x far=%08x%08x cbar=%08x sctlr=%08x\n"
           "uic pa=%08x dl=%08x net=%08x tr=%08x dme=%08x\n"
           "cap=%08x\n"
           "rsp=%08x %08x\n"
           "%s",
           t.ocs, t.response, t.scsi, phys, hcsBefore, read32 (hc, 0x30), smmuReport[0], smmuReport[1],
           read32 (hc, 0x20), read32 (hc, 0x58), read32 (hc, 0x60), read32 (hc, 0x34),
           read32 (hc, 0x4c), gLastPolls,
           dw0, static_cast<uint32_t> (phys + ucdOffset), gLastRb0, gLastRb8,           gfsr, cbfsr, farHi, farLo, cbBar, cbSctlr,
           read32 (hc, 0x38), read32 (hc, 0x3c), read32 (hc, 0x40), read32 (hc, 0x44),
           read32 (hc, 0x48),
           read32 (hc, 0x00),
           getBe32 (rsp0), getBe32 (rsp0 + 4), mcq);
  }
  uint32_t hcsAfter = 0;
  const unsigned maxLun = 6;
  uint32_t status[maxLun], last[maxLun], blockSize[maxLun], senseOf[maxLun];
  for (unsigned lun = 0; lun < maxLun; lun++) {
    status[lun] = 0xee; last[lun] = 0; blockSize[lun] = 0; senseOf[lun] = 0;
    uint8_t tur[6] = { 0x00, 0, 0, 0, 0, 0 };
    UfsResult r = { 0xf, 0xff, 0xff, 0, 0, 0 };
    for (unsigned attempt = 0; attempt < 3; attempt++) {
      r = command (static_cast<uint8_t> (lun), tur, 6, false, 0);
      if (lun == 0 && attempt == 0 && r.ocs == 0xf) {
        // Never completed: stop here and report instead of waiting out every command.
        const uint8_t *rsp = virt + ucdOffset + rspOffset;
        panic ("NATIVE_UFS adopt: first command did not complete\n"
               "phys=%llx hcs=%08x->%08x\n"
               "is=%08x db=%08x rsr=%08x ba=%08x\n"
               "utrd2=%08x rsp=%08x %08x",
               phys, hcsBefore, read32 (hc, 0x30),
               read32 (hc, 0x20), read32 (hc, 0x58), read32 (hc, 0x60), read32 (hc, 0x50),
               *reinterpret_cast<volatile uint32_t *> (virt + 8), getBe32 (rsp), getBe32 (rsp + 4));
      }
      if (r.ocs == 0 && r.scsi == 0) {
        break;
      }
    }
    uint8_t cap[10] = { 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    r = command (static_cast<uint8_t> (lun), cap, 10, true, 8);
    status[lun] = (r.ocs << 16) | (r.response << 8) | r.scsi;
    senseOf[lun] = r.sense;
    if (r.ocs == 0 && r.scsi == 0) {
      last[lun] = getBe32 (virt + dataOffset);
      blockSize[lun] = getBe32 (virt + dataOffset + 4);
    }
  }
  if (level == 9) {
    // Read-only GPT survey of LUN 0 (4096-byte blocks): header fields and CRCs for before/after
    // comparison, then every non-empty entry among the first 64. Nothing is written.
    if (blockSize[0] != 4096) {
      panic ("NATIVE_UFS gpt: unexpected LUN0 block size %08x", blockSize[0]);
    }
    uint8_t rd[10] = { 0x28, 0, 0, 0, 0, 1, 0, 0, 1, 0 };
    UfsResult h = command (0, rd, 10, true, 4096);
    if (h.ocs != 0 || h.scsi != 0) {
      panic ("NATIVE_UFS gpt: header read failed ocs=%x resp=%x scsi=%x", h.ocs, h.response, h.scsi);
    }
    const uint8_t *hdr = virt + dataOffset;
    auto le32 = [] (const uint8_t *at) -> uint32_t {
      return uint32_t (at[0]) | (uint32_t (at[1]) << 8) | (uint32_t (at[2]) << 16) | (uint32_t (at[3]) << 24);
    };
    auto le64 = [&] (const uint8_t *at) -> uint64_t {
      return uint64_t (le32 (at)) | (uint64_t (le32 (at + 4)) << 32);
    };
    uint32_t sig0 = le32 (hdr), sig1 = le32 (hdr + 4), hdrCrc = le32 (hdr + 16), entCrc = le32 (hdr + 88);
    uint64_t curLba = le64 (hdr + 24), bakLba = le64 (hdr + 32), firstUsable = le64 (hdr + 40), lastUsable = le64 (hdr + 48);
    uint64_t entLba = le64 (hdr + 72);
    uint32_t entCount = le32 (hdr + 80), entSize = le32 (hdr + 84);
    uint32_t guid0 = le32 (hdr + 56), guid1 = le32 (hdr + 60), guid2 = le32 (hdr + 64), guid3 = le32 (hdr + 68);
    if (sig0 != 0x20494645U || sig1 != 0x54524150U || entSize != 128 || entLba > 64) {
      panic ("NATIVE_UFS gpt: unexpected header sig=%08x %08x esz=%u elba=%llx", sig0, sig1, entSize, entLba);
    }
    uint32_t shown = entCount > 64 ? 64 : entCount;
    uint32_t blocks = (shown * 128 + 4095) / 4096;
    uint8_t rd2[10] = { 0x28, 0, 0, 0, 0, uint8_t (entLba), 0, 0, uint8_t (blocks), 0 };
    UfsResult e = command (0, rd2, 10, true, blocks * 4096);
    if (e.ocs != 0 || e.scsi != 0) {
      panic ("NATIVE_UFS gpt: entries read failed ocs=%x scsi=%x", e.ocs, e.scsi);
    }
    const uint8_t *ents = virt + dataOffset;
    static char text[3400];
    int used = snprintf (text, sizeof (text),
                         "NATIVE_UFS gpt LUN0 hdr crc=%08x ents crc=%08x cnt=%u\n"
                         "cur=%llx backup=%llx usable=%llx..%llx elba=%llx\n"
                         "disk guid %08x %08x %08x %08x last=%08x\n",
                         hdrCrc, entCrc, entCount, curLba, bakLba, firstUsable, lastUsable, entLba,
                         guid0, guid1, guid2, guid3, last[0]);
    unsigned column = 0, listed = 0;
    for (uint32_t index = 0; index < shown && used < int (sizeof (text)) - 80; index++) {
      const uint8_t *entry = ents + index * 128;
      bool empty = true;
      for (unsigned b = 0; b < 16; b++) {
        if (entry[b]) { empty = false; break; }
      }
      if (empty) {
        continue;
      }
      char name[14];
      for (unsigned c = 0; c < 13; c++) {
        uint8_t lo = entry[56 + 2 * c], hi = entry[57 + 2 * c];
        name[c] = (hi == 0 && lo >= 32 && lo < 127) ? char (lo) : (lo ? '?' : 0);
        if (!name[c]) { name[c] = 0; break; }
      }
      name[13] = 0;
      used += snprintf (text + used, sizeof (text) - used, "%u:%s %llx-%llx%s", index, name,
                        le64 (entry + 32), le64 (entry + 40), (++column % 3) ? "  " : "\n");
      listed++;
    }
    snprintf (text + used, sizeof (text) - used, "\nlisted=%u", listed);
    panic ("%s", text);
  }
  // INQUIRY of LUN 0: vendor/product bytes 8..23.
  uint8_t inquiry[6] = { 0x12, 0, 0, 0, 36, 0 };
  UfsResult inq = command (0, inquiry, 6, true, 36);
  uint32_t inqWord[4] = { 0, 0, 0, 0 };
  for (unsigned index = 0; index < 4; index++) {
    inqWord[index] = getBe32 (virt + dataOffset + 8 + index * 4);
  }
  // LBA 1 of LUN 0 holds the GPT header when the block size is 4096 (or 512).
  uint32_t gptStatus = 0xee, sig0 = 0, sig1 = 0;
  if (blockSize[0] >= 512 && blockSize[0] <= 4096) {
    uint8_t read10[10] = { 0x28, 0, 0, 0, 0, 1, 0, 0, 1, 0 };
    UfsResult g = command (0, read10, 10, true, blockSize[0]);
    gptStatus = (g.ocs << 16) | (g.response << 8) | g.scsi;
    if (g.ocs == 0 && g.scsi == 0) {
      sig0 = *reinterpret_cast<uint32_t *> (virt + dataOffset);
      sig1 = *reinterpret_cast<uint32_t *> (virt + dataOffset + 4);
    }
  }
  hcsAfter = read32 (hc, 0x30);
  panic ("NATIVE_UFS adopt+read\n"
         "uic pwr=%08x st=%08x lanes=%08x st=%08x exit=%08x\n"
         "hcs %08x->%08x pwr %08x->%08x ssu=%06x\n"
         "L0 st=%06x last=%08x bs=%08x sns=%06x\n"
         "L1 st=%06x last=%08x bs=%08x sns=%06x\n"
         "L2 st=%06x last=%08x bs=%08x sns=%06x\n"
         "L3 st=%06x last=%08x bs=%08x sns=%06x\n"
         "L4 st=%06x last=%08x bs=%08x sns=%06x\n"
         "L5 st=%06x last=%08x bs=%08x sns=%06x\n"
         "inq st=%06x %08x %08x %08x %08x\n"
         "inq sns=%06x slen=%x seg=%x\n"
         "rsp0 %08x %08x %08x %08x\n"
         "rsp8 %08x %08x %08x %08x\n"
         "gpt st=%06x sig=%08x %08x",
         uicPwr, uicPwrStatus, uicLanes, uicLanesStatus, uicExit,
         hcsBefore, hcsAfter, powerBefore, powerAfter, wakeStatus,
         status[0], last[0], blockSize[0], senseOf[0], status[1], last[1], blockSize[1], senseOf[1],
         status[2], last[2], blockSize[2], senseOf[2], status[3], last[3], blockSize[3], senseOf[3],
         status[4], last[4], blockSize[4], senseOf[4], status[5], last[5], blockSize[5], senseOf[5],
         (inq.ocs << 16) | (inq.response << 8) | inq.scsi, inqWord[0], inqWord[1], inqWord[2], inqWord[3],
         inq.sense, inq.senseLen, inq.seg,
         getBe32 (virt + ucdOffset + rspOffset), getBe32 (virt + ucdOffset + rspOffset + 4),
         getBe32 (virt + ucdOffset + rspOffset + 8), getBe32 (virt + ucdOffset + rspOffset + 12),
         getBe32 (virt + ucdOffset + rspOffset + 32), getBe32 (virt + ucdOffset + rspOffset + 36),
         getBe32 (virt + ucdOffset + rspOffset + 40), getBe32 (virt + ucdOffset + rspOffset + 44),
         gptStatus, sig0, sig1);
}

// Read-only dump of the request list the firmware left (native-ufs-probe=6): shows where
// the firmware placed its descriptors, to compare with an allocation the controller's DMA
// is known to reach. Nothing is written and no register is touched except reads.
static uint32_t dumpWord (const volatile uint32_t *base, unsigned index)
{
  return base[index];
}

void NativeUfs::dumpFirmwareList (void)
{
  OSSharedPtr<IOMemoryMap> hcMap, listMap, ucdMap;
  uintptr_t hc = mapPhys (UFS_HC_BASE, UFS_HC_SIZE, hcMap);
  if (!hc) {
    panic ("NATIVE_UFS dump: controller map failed");
  }
  uint64_t listPhys = (uint64_t (read32 (hc, 0x54)) << 32) | read32 (hc, 0x50);
  uint32_t db = read32 (hc, 0x58), rsr = read32 (hc, 0x60), hcs = read32 (hc, 0x30);
  IOMemoryDescriptor *listDesc = IOMemoryDescriptor::withPhysicalAddress (listPhys, 4096, kIODirectionIn);
  if (!listDesc) {
    panic ("NATIVE_UFS dump: list descriptor failed phys=%llx", listPhys);
  }
  listMap = OSSharedPtr<IOMemoryMap> (listDesc->map (kIOMapAnywhere), OSNoRetain);
  listDesc->release ();
  if (!listMap) {
    panic ("NATIVE_UFS dump: list map failed phys=%llx", listPhys);
  }
  const volatile uint32_t *utrd = reinterpret_cast<const volatile uint32_t *> (listMap->getVirtualAddress ());
  uint64_t ucdPhys = (uint64_t (dumpWord (utrd, 5)) << 32) | (dumpWord (utrd, 4) & 0xffffff80U);
  uint32_t ucd0 = 0, ucd1 = 0, ucd2 = 0, ucd3 = 0;
  if (ucdPhys) {
    IOMemoryDescriptor *ucdDesc = IOMemoryDescriptor::withPhysicalAddress (ucdPhys, 4096, kIODirectionIn);
    if (ucdDesc) {
      ucdMap = OSSharedPtr<IOMemoryMap> (ucdDesc->map (kIOMapAnywhere), OSNoRetain);
      ucdDesc->release ();
      if (ucdMap) {
        const volatile uint32_t *ucd = reinterpret_cast<const volatile uint32_t *> (ucdMap->getVirtualAddress ());
        ucd0 = dumpWord (ucd, 0); ucd1 = dumpWord (ucd, 1); ucd2 = dumpWord (ucd, 2); ucd3 = dumpWord (ucd, 3);
      }
    }
  }
  panic ("NATIVE_UFS dump: firmware list\n"
         "list=%llx hcs=%08x db=%08x rsr=%08x\n"
         "utrd0 %08x %08x %08x %08x\n"
         "utrd0 %08x %08x %08x %08x\n"
         "utrd1 %08x %08x %08x %08x\n"
         "ucd=%llx %08x %08x %08x %08x",
         listPhys, hcs, db, rsr,
         dumpWord (utrd, 0), dumpWord (utrd, 1), dumpWord (utrd, 2), dumpWord (utrd, 3),
         dumpWord (utrd, 4), dumpWord (utrd, 5), dumpWord (utrd, 6), dumpWord (utrd, 7),
         dumpWord (utrd, 8), dumpWord (utrd, 9), dumpWord (utrd, 10), dumpWord (utrd, 11),
         ucdPhys, ucd0, ucd1, ucd2, ucd3);
}

// Read-only dump of the apps SMMU (apps_smmu@15000000) global register space and the stream
// match entry for the UFS stream ID 0x60 (native-ufs-probe=7). SMMUv2 layout: sCR0 0x0,
// IDR0 0x20, sGFSR 0x48, SMR(n) 0x800+4n, S2CR(n) 0xc00+4n. Nothing is written.
void NativeUfs::inspectSmmu (void)
{
  OSSharedPtr<IOMemoryMap> map;
  uintptr_t smmu = mapPhys (0x15000000ULL, 0x1000, map);
  if (!smmu) {
    panic ("NATIVE_UFS smmu: map failed");
  }
  uint32_t scr0 = read32 (smmu, 0x00), idr0 = read32 (smmu, 0x20), idr1 = read32 (smmu, 0x24);
  uint32_t gfsr = read32 (smmu, 0x48);
  unsigned count = idr0 & 0xff;
  if (count > 128) {
    count = 128;
  }
  unsigned match = 0xffff, valid = 0, typeCount[4] = { 0, 0, 0, 0 };
  uint32_t matchSmr = 0, matchS2cr = 0;
  for (unsigned index = 0; index < count; index++) {
    uint32_t smr = read32 (smmu, 0x800 + 4 * index);
    uint32_t s2cr = read32 (smmu, 0xc00 + 4 * index);
    if (smr & 0x80000000U) {
      valid++;
      typeCount[(s2cr >> 16) & 3]++;
      uint32_t mask = (smr >> 16) & 0x7fff;
      if (match == 0xffff && (((smr ^ 0x60U) & 0x7fffU) & ~mask) == 0) {
        match = index;
        matchSmr = smr;
        matchS2cr = s2cr;
      }
    }
  }
  panic ("NATIVE_UFS smmu\n"
         "scr0=%08x idr0=%08x idr1=%08x gfsr=%08x\n"
         "nsmr=%u valid=%u types t0=%u t1=%u t2=%u\n"
         "sid0x60 match=%x smr=%08x s2cr=%08x",
         scr0, idr0, idr1, gfsr, count, valid, typeCount[0], typeCount[1], typeCount[2],
         match, matchSmr, matchS2cr);
}

// Read-only context-bank survey (native-ufs-probe=8): CBAR/CBA2R in gr1 and SCTLR/TTBR0 in the
// context banks, to find a bank the hypervisor leaves to us. Nothing is written.
void NativeUfs::inspectContextBanks (void)
{
  OSSharedPtr<IOMemoryMap> map;
  uintptr_t smmu = mapPhys (0x15000000ULL, 0xd0000, map);
  if (!smmu) {
    panic ("NATIVE_UFS cb: map failed");
  }
  uint32_t idr1 = read32 (smmu, 0x24);
  unsigned numCb = idr1 & 0xff;
  unsigned globalPages = 1U << (((idr1 >> 28) & 7) + 1);
  if (globalPages > 128 || numCb > 128 || (globalPages + numCb) * 0x1000 > 0xd0000) {
    panic ("NATIVE_UFS cb: unexpected geometry idr1=%08x", idr1);
  }
  unsigned cbarZero = 0, sctlrZero = 0, mSet = 0, bothZero = 0;
  uint32_t firstZero = 0xffff;
  for (unsigned index = 0; index < numCb; index++) {
    uint32_t cbar = read32 (smmu, 0x1000 + 4 * index);
    uint32_t sctlr = read32 (smmu + (globalPages + index) * 0x1000, 0x0);
    cbarZero += cbar == 0;
    sctlrZero += sctlr == 0;
    mSet += (sctlr & 1) != 0;
    if (cbar == 0 && sctlr == 0) {
      bothZero++;
      if (firstZero == 0xffff) {
        firstZero = index;
      }
    }
  }
  uintptr_t cb0 = smmu + globalPages * 0x1000, cb1 = cb0 + 0x1000, cbLast = cb0 + (numCb - 1) * 0x1000;
  panic ("NATIVE_UFS cb survey\n"
         "ncb=%u pages=%u cbar0=%u sctlr0=%u msset=%u both0=%u first0=%x\n"
         "cbar[0]=%08x cbar[1]=%08x cbar[%u]=%08x\n"
         "sctlr[0]=%08x sctlr[1]=%08x sctlr[last]=%08x\n"
         "cba2r[0]=%08x cba2r[1]=%08x",
         numCb, globalPages, cbarZero, sctlrZero, mSet, bothZero, firstZero,
         read32 (smmu, 0x1000), read32 (smmu, 0x1004), numCb - 1, read32 (smmu, 0x1000 + 4 * (numCb - 1)),
         read32 (cb0, 0), read32 (cb1, 0), read32 (cbLast, 0),
         read32 (smmu, 0x1800), read32 (smmu, 0x1804));
}

// ======================================================================================
// Root media: the live controller as a read-only block device.
//
// One shared host (a single UTP request slot, polled; interrupts stay masked) serves every
// block device. The DMA region is ordinary cached, physically contiguous kernel memory with
// explicit cache maintenance around each command (the stream is mapped through the reserved
// apps_smmu bypass context bank by smmuBypassStream, so physical addresses are used as-is).
// Nothing here writes to the storage: only INQUIRY-class and READ(10) commands are built, plus
// the single START STOP UNIT that wakes the device from the power-down the firmware left.
// ======================================================================================
namespace {
#include "RootTransportLayout.h"

struct UfsHost {
  OSSharedPtr<IOMemoryMap> hcMap, smmuMap;
  uintptr_t hc;
  IOBufferMemoryDescriptor *dma;
  uint8_t *virt;
  uint64_t phys;
  IOLock *lock;
  bool ready;
  bool poisoned = false;
  uintptr_t smmu = 0, cb = 0;
};
UfsHost gHost = { OSSharedPtr<IOMemoryMap> (), OSSharedPtr<IOMemoryMap> (), 0, nullptr, nullptr, 0, nullptr, false };

static IOReturn recordTransport (IOReturn result, uint32_t ocs)
{
  gRootDiagnostic.result = static_cast<uint32_t> (result);
  gRootDiagnostic.ocs = ocs;
  if (result != kIOReturnSuccess && gRootDiagnosticEnabled) {
    // Read only the already mapped HC's status/registers used by this driver.
    gRootDiagnostic.hcs = read32 (gHost.hc, 0x30);
    gRootDiagnostic.hce = read32 (gHost.hc, 0x34);
    gRootDiagnostic.doorbell = read32 (gHost.hc, 0x58);
    gRootDiagnostic.interrupt = read32 (gHost.hc, 0x20);
    gRootDiagnostic.list = uint64_t (read32 (gHost.hc, 0x50)) |
                          (uint64_t (read32 (gHost.hc, 0x54)) << 32);
    gRootDiagnostic.runStop = read32 (gHost.hc, 0x60);
    gRootDiagnostic.ahit = read32 (gHost.hc, 0x18);
    for (unsigned i = 0; i < 5; ++i) gRootDiagnostic.uic[i] = read32 (gHost.hc, 0x38 + 4 * i);
    if (gHost.smmu && gHost.cb) {
      gRootDiagnostic.gfsr = read32 (gHost.smmu, 0x48);
      gRootDiagnostic.gsyn = read32 (gHost.smmu, 0x54);
      gRootDiagnostic.fsr = read32 (gHost.cb, 0x58);
      gRootDiagnostic.fsyn = read32 (gHost.cb, 0x68);
      gRootDiagnostic.far = uint64_t (read32 (gHost.cb, 0x60)) |
                            (uint64_t (read32 (gHost.cb, 0x64)) << 32);
    }
  }
  return result;
}

static void cacheCivac (const void *start, size_t length)
{
  uintptr_t address = reinterpret_cast<uintptr_t> (start) & ~uintptr_t (63);
  uintptr_t end = reinterpret_cast<uintptr_t> (start) + length;
  for (; address < end; address += 64) {
    __asm__ volatile ("dc civac, %0" :: "r" (address) : "memory");
  }
  __asm__ volatile ("dsb sy" ::: "memory");
}

// Device-owned cache lines must not be written back over DMA results. All
// callers use complete 64-byte lines from the dedicated page-aligned buffer.
static void cacheInvalidate (const void *start, size_t length)
{
  __asm__ volatile ("dsb sy" ::: "memory");
  uintptr_t address = reinterpret_cast<uintptr_t> (start);
  for (uintptr_t end = address + length; address < end; address += 64) {
    __asm__ volatile ("dc ivac, %0" :: "r" (address) : "memory");
  }
  __asm__ volatile ("dsb sy" ::: "memory");
}

static void hostWrite (unsigned offset, uint32_t value)
{
  __asm__ volatile ("dsb sy" ::: "memory");
  write32 (gHost.hc, offset, value);
  __asm__ volatile ("dsb sy" ::: "memory");
}

#include "RootQueue.h"

#include "RootCommands.h"

// Bring the live controller up for reading: map, SMMU pass-through for the UFS stream, take over
// the request list, wake the device from UFS-PowerDown and wait for LUN 0 to be ready.
// `why` receives a short reason on failure.
static bool hostBringUp (const char **why)
{
  if (gHost.poisoned) {
    *why = "transport quarantined";
    return false;
  }
  if (gHost.ready) {
    return true;
  }
  gRootDiagnostic.stage = "controller map";
  gHost.hc = mapPhys (UFS_HC_BASE, UFS_HC_SIZE, gHost.hcMap);
  if (!gHost.hc) {
    *why = "controller map";
    return false;
  }
  if (gRootDiagnosticEnabled) {
    gRootDiagnostic.cap = read32 (gHost.hc, 0x00);
    gRootDiagnostic.version = read32 (gHost.hc, 0x08);
    gRootDiagnostic.hcs = read32 (gHost.hc, 0x30);
    gRootDiagnostic.hce = read32 (gHost.hc, 0x34);
  }
  gRootDiagnostic.stage = "DMA allocation";
  gHost.dma = IOBufferMemoryDescriptor::inTaskWithOptions (
    kernel_task, static_cast<IOOptionBits> (kIODirectionInOut) | static_cast<IOOptionBits> (kIOMemoryPhysicallyContiguous) |
    static_cast<IOOptionBits> (kIOMemoryMapperNone),
    kDmaBytes, 4096);
  gHost.lock = IOLockAlloc ();
  if (!gHost.dma || !gHost.lock) {
    *why = "dma buffer";
    return false;
  }
  gHost.virt = static_cast<uint8_t *> (gHost.dma->getBytesNoCopy ());
  IOByteCount contiguous = 0;
  gHost.phys = gHost.dma->getPhysicalSegment (0, &contiguous, kIOMemoryMapperNone);
  gRootDiagnostic.dma = gHost.phys;
  gRootDiagnostic.contiguous = contiguous;
  if (!gHost.virt || !gHost.phys || (gHost.phys & 4095) || contiguous < kDmaBytes ||
      (!(read32 (gHost.hc, 0) & 0x01000000) && gHost.phys + kDmaBytes > 0x100000000ULL)) {
    *why = "DMA address/contiguity";
    return false;
  }
  bzero (gHost.virt, kDmaBytes);
  cacheCivac (gHost.virt, kDmaBytes);
  uint32_t report[3] = { 0, 0, 0 };
  gRootDiagnostic.stage = "SMMU and request list";
  if (!hostStopQueue (why)) return false;
  smmuBypassStream (0x60, gHost.smmuMap, report, gHost.phys, kDmaBytes);
  gHost.smmu = gHost.smmuMap->getVirtualAddress ();
  uint32_t idr1 = read32 (gHost.smmu, 0x24);
  gHost.cb = gHost.smmu + ((1U << (((idr1 >> 28) & 7) + 1)) + (report[0] >> 16)) * 4096;
  if (gRootDiagnosticEnabled) {
    gRootDiagnostic.gfsrBefore = read32 (gHost.smmu, 0x48);
    gRootDiagnostic.fsrBefore = read32 (gHost.cb, 0x58);
    gRootDiagnostic.smmuId2 = read32 (gHost.smmu, 0x28);
    gRootDiagnostic.cba2r = read32 (gHost.smmu, 0x1800 + 4 * (report[0] >> 16));
  }
  if (!hostStartQueue (why)) return false;
  // Prime the UIC path: the controller won't service UTP transfer list requests until it
  // completes at least one UIC command after taking over from firmware. Without this, the
  // doorbell sticks at 1, IS stays zero, and NOP OUT never completes (probe=10/root mode
  // failed here while probe=9's adopt path succeeded).
  write32 (gHost.hc, 0x20, 0xffffffffU);
  if (!NativeUfs::waitSet (gHost.hc, 0x30, 0x8, 100000)) { *why = "uic readiness"; return false; }
  write32 (gHost.hc, 0x94, (0x1571U << 16)); write32 (gHost.hc, 0x98, 0); write32 (gHost.hc, 0x9c, 0);
  write32 (gHost.hc, 0x90, 0x01);           // DME_GET PA_PWR_MODE
  if (!NativeUfs::waitSet (gHost.hc, 0x20, 0x400, 200000)) { *why = "uic dme_get pwr"; return false; }
  write32 (gHost.hc, 0x94, (0x1560U << 16)); write32 (gHost.hc, 0x98, 0); write32 (gHost.hc, 0x9c, 0);
  write32 (gHost.hc, 0x90, 0x01);           // DME_GET PA_ActiveTxDataLanes
  if (!NativeUfs::waitSet (gHost.hc, 0x20, 0x400, 200000)) { *why = "uic dme_get lanes"; return false; }
  write32 (gHost.hc, 0x90, 0x18);           // DME_HIBERNATE_EXIT
  if (!NativeUfs::waitSet (gHost.hc, 0x20, 0x400, 200000)) { *why = "uic hibernate exit"; return false; }
  write32 (gHost.hc, 0x20, 0xffffffffU);
  gRootDiagnostic.stage = "NOP OUT";
  // NOP OUT proves the transport before touching the SCSI layer.
  if (hostNop () != kIOReturnSuccess) {
    gHost.ready = false;
    *why = "NOP OUT";
    return false;
  }
  uint32_t power = 0;
  gRootDiagnostic.stage = "power mode query";
  if (hostQueryRead (0x03, 0x02, &power) != kIOReturnSuccess) {
    gHost.ready = false;
    *why = "power mode query";
    return false;
  }
  gRootDiagnostic.power = power;
  if ((power & 0xff) != 0x11) {
    gRootDiagnostic.stage = "device wake";
    // START STOP UNIT, power condition ACTIVE, to the UFS Device well-known LU (as Linux resumes).
    uint8_t ssu[6] = { 0x1b, 0, 0, 0, 0x10, 0 };
    uint32_t scsi = 0;
    if (hostScsi (0xd0, ssu, 6, false, 0, 500000, &scsi) != kIOReturnSuccess) {
      gHost.ready = false;
      *why = "wake (START STOP UNIT)";
      return false;
    }
    IOSleep (50);
  }
  gRootDiagnostic.stage = "LUN ready";
  for (unsigned attempt = 0; attempt < 20; attempt++) {
    uint8_t tur[6] = { 0, 0, 0, 0, 0, 0 };
    uint32_t scsi = 0;
    if (hostScsi (0, tur, 6, false, 0, 200000, &scsi) == kIOReturnSuccess) {
      gHost.ready = true;
      return true;
    }
    if (gHost.poisoned) break;
    IOSleep (10);
  }
  gHost.ready = false;
  *why = "LUN 0 not ready";
  return false;
}
}

namespace {
static uint32_t be32w (const uint8_t *at)
{
  return (uint32_t (at[0]) << 24) | (uint32_t (at[1]) << 16) | (uint32_t (at[2]) << 8) | at[3];
}

// The GPT partition that carries the root image and its manifest. The sheng transport keeps the root
// in "userdata"; miro has a dedicated partition for it, so the root image never shares a partition
// with anything the phone flashes or formats. One name, one place: the boot argument below only
// chooses whether the root is published, never where it lives. K80 Pro's factory table has no such
// entry; findRootLayout then uses the head of "userdata", the block fastboot flash userdata writes.
static const char kRootPartition[] = "linux";

#include "RootLayout.h"
#ifdef NATIVE_UFS_BLOCK_DEVICE
// The window reader and the publication plan belong to the block-device half: their helpers are static
// functions in a header, so leaving the headers in a build that has no block device makes the
// compiler report them as unused (-Wunused-function, -Wunneeded-internal-declaration).
#include "RootWindowRead.h"
#include "RootPublication.h"
#endif

}

// ---- Persistent Data store window (miro-data-persist) ------------------------------------------------------------
// The only part of this driver that ever writes. The consumer (NativeRamBlock's overlay) asks for a window of the given
// size at the END of the root partition. Mode 1 (boot argument miro-data-persist=1) only reads: it proves the window and
// lets the consumer inspect its header. Mode 2 additionally opens the write window. A window is refused unless it lies inside
// the partition, clear of the flashed root image plus a 4 MiB guard, and is at most a quarter of the partition.
namespace {
struct StoreGeometry {
  bool valid;
  uint64_t first, last, imageBytes, entriesLba;
  uint32_t headerCrc, entriesCrc, entryCount;
};
StoreGeometry gStoreGeometry;
uint64_t gStoreRegionFirst, gStoreRegionBlocks;
// The board partition that carries our panic record and bring-up probe: MIUI's own mtdoops device, 16 MiB of crash logs and
// nothing else. A record anywhere else would have to be a partition somebody's data lives in.
constexpr const char *kPanicPartition = "oops";
constexpr uint64_t kPanicRecordBlocks = 8;      // 32 KiB of panic text
constexpr uint64_t kProbeBlocks = 1;            // one bring-up probe block, below the record
constexpr uint64_t kPanicWindowBlocks = kPanicRecordBlocks + kProbeBlocks;
// miro l23: the rest of the same partition carries the full kernel console, so one readback of `oops` yields the
// whole log. 64 KiB slots in a ring, newest written last, each with its own magic and length.
constexpr uint64_t kOopsLogBlocks = 3968;       // 15.5 MiB, below the panic window
constexpr uint64_t kOopsSlotBlocks = 16;        // one 64 KiB record per round

// The panic window, opened once from the crash-log partition's tail. No fallback: if the named partition is absent or too
// small, the record is not written at all.
static bool panicWindowOpen (uint64_t *firstOut, const char **why)
{
  if (gPanicOpen) {
    *firstOut = gPanicFirst;
    return true;
  }
  uint64_t first = 0, last = 0;
  const char *reason = "";
  if (!rootNamedRange (kPanicPartition, &first, &last, &reason)) {
    if (why) *why = reason;
    return false;
  }
  if (last - first + 1 < kPanicWindowBlocks) {
    if (why) *why = "crash-log partition smaller than the record";
    return false;
  }
  const uint64_t at = last + 1 - kPanicWindowBlocks;
  if (!hostPanicOpen (at, kPanicWindowBlocks)) {
    if (why) *why = "panic window refused";
    return false;
  }
  *firstOut = at;
  return true;
}

// The full-log window: the head of the same crash-log partition, opened once.
static bool oopsLogWindow (uint64_t *firstOut, const char **why)
{
  static bool open = false;
  static uint64_t first = 0;
  if (open) {
    *firstOut = first;
    return true;
  }
  uint64_t partFirst = 0, partLast = 0;
  const char *reason = "";
  if (!rootNamedRange (kPanicPartition, &partFirst, &partLast, &reason)) {
    if (why) *why = reason;
    return false;
  }
  if (partLast - partFirst + 1 < kPanicWindowBlocks + kOopsLogBlocks) {
    if (why) *why = "crash-log partition too small for the full log";
    return false;
  }
  if (!hostOopsOpen (partFirst, kOopsLogBlocks)) {
    if (why) *why = "full-log window refused";
    return false;
  }
  first = partFirst;
  open = true;
  *firstOut = first;
  return true;
}

// Append one round: a header block (magic, sequence, length, slot), then the text.
extern "C" int native_ufs_oops_log_write (uint32_t sequence, const char *text, uint32_t length)
{
  if (!text || !length) return 0;
  const char *why = "";
  uint64_t first = 0;
  if (!oopsLogWindow (&first, &why)) return 0;
  const uint64_t slots = kOopsLogBlocks / kOopsSlotBlocks;
  const uint64_t slot = sequence % slots;
  const uint64_t at = first + slot * kOopsSlotBlocks;
  const uint32_t room = (uint32_t)((kOopsSlotBlocks - 1) * 4096);
  if (length > room) length = room;
  static uint8_t header[4096];
  for (unsigned i = 0; i < sizeof (header); ++i) header[i] = 0;
  const char magic[8] = {'O', 'O', 'P', 'S', 'L', 'O', 'G', '1'};
  for (unsigned i = 0; i < 8; ++i) header[i] = uint8_t (magic[i]);
  memcpy (header + 8, &sequence, 4);
  memcpy (header + 12, &length, 4);
  uint32_t slot32 = static_cast<uint32_t> (slot);
  memcpy (header + 16, &slot32, 4);
  if (hostWrite4k (0, at, 1, header) != kIOReturnSuccess) return 0;
  for (uint32_t done = 0; done < length; done += 4096) {
    uint32_t chunk = length - done;
    if (chunk > 4096) chunk = 4096;
    static uint8_t body[4096];
    for (unsigned i = 0; i < sizeof (body); ++i) body[i] = 0;
    memcpy (body, text + done, chunk);
    if (hostWrite4k (0, at + 1 + done / 4096, 1, body) != kIOReturnSuccess) return 0;
  }
  hostSyncCache ();
  return 1;
}

// The diagnostic log (miro-log=1): always the last 32 MiB of the root partition, so the Data store's place does not depend on logging.
constexpr uint64_t kLogReserveBlocks = 8192;
// The log's export region: the 256 MiB just below the log window, for a board log writer (not part of this port). It is
// part of the log's write window, so the store ends below both.
constexpr uint64_t kExportBlocks = 65536;
uint64_t gLogRegionFirst, gLogRegionBlocks;
// The reserve at the end of the root partition that the Data store stays clear of: the log and its export region sit inside it
// (miro-log=1), the rest is unused. miro-log-reserve=<MiB>, default 1024; 0 gives all the room to Data and is only accepted with the
// log off. The store's place
// depends on it, so an image keeps one value for its whole life (a different value moves the store: the old one is read as blank).
constexpr uint32_t kDefaultReserveMiB = 1024;
static uint64_t reserveBlocks (void)
{
  static int64_t cached = -1;
  if (cached < 0) {
    uint32_t mib = kDefaultReserveMiB;
    if (!PE_parse_boot_argn ("miro-log-reserve", &mib, sizeof (mib)) || mib > 16384) mib = kDefaultReserveMiB;
    cached = static_cast<int64_t> (mib) * 256;   // 4096-byte blocks
  }
  return static_cast<uint64_t> (cached);
}

static unsigned storeMode (void)
{
  static int cached = -1;
  if (cached < 0) {
    uint32_t value = 0;
    if (!PE_parse_boot_argn ("miro-data-persist", &value, sizeof (value)) || value > 2) value = 0;
    cached = static_cast<int> (value);
  }
  return static_cast<unsigned> (cached);
}
}

extern "C" unsigned native_ufs_store_mode (void) { return storeMode (); }

// Returns 1 and the first absolute 4096-byte LBA of the window, 0 on refusal (reason in *why, may be null).
extern "C" int native_ufs_store_open (uint64_t blocks, uint64_t *firstLba, const char **why)
{
  const char *reason = "";
  int ok = 0;
  if (!storeMode ()) reason = "off";
  else if (!gStoreGeometry.valid || !gHost.ready) reason = "no root layout";
  else if (gStoreRegionBlocks) reason = "already open";
  else {
    uint64_t partition = gStoreGeometry.last - gStoreGeometry.first + 1;
    uint64_t imageBlocks = (gStoreGeometry.imageBytes + 4095) / 4096;
    const uint64_t reserve = reserveBlocks ();
    if (!blocks || reserve >= partition || blocks > partition - reserve) reason = "size";
    else if (partition - blocks - reserve < imageBlocks + 1024) reason = "overlaps the root image";
    else {
      // The reserve at the end of the partition belongs to the diagnostic log (its last 32 MiB, with the export region below them);
      // the store ends just below it.
      uint64_t first = gStoreGeometry.last + 1 - reserve - blocks;
      if (storeMode () == 2 && !hostStoreOpen (first, blocks)) reason = "window refused";
      else {
        gStoreRegionFirst = first;
        gStoreRegionBlocks = blocks;
        if (firstLba) *firstLba = first;
        ok = 1;
      }
    }
  }
  if (why) *why = reason;
  return ok;
}

extern "C" int native_ufs_store_read (uint64_t rel, uint32_t blocks, void *dst)
{
  if (!gStoreRegionBlocks || !dst || !blocks || rel >= gStoreRegionBlocks || blocks > gStoreRegionBlocks - rel) return 0;
  while (blocks) {
    uint32_t chunk = blocks > kDataMax / 4096 ? static_cast<uint32_t> (kDataMax / 4096) : blocks;
    if (hostRead4k (0, gStoreRegionFirst + rel, chunk, dst) != kIOReturnSuccess) return 0;
    dst = static_cast<uint8_t *> (dst) + chunk * 4096ULL;
    rel += chunk;
    blocks -= chunk;
  }
  return 1;
}

extern "C" int native_ufs_store_write (uint64_t rel, uint32_t blocks, const void *src)
{
  if (storeMode () != 2 || !gStoreRegionBlocks || !src || !blocks || rel >= gStoreRegionBlocks ||
      blocks > gStoreRegionBlocks - rel) return 0;
  while (blocks) {
    uint32_t chunk = blocks > kDataMax / 4096 ? static_cast<uint32_t> (kDataMax / 4096) : blocks;
    // Forced unit access: the block is on the medium when the command completes, so the overlay needs no cache flush.
    if (hostWrite4k (0, gStoreRegionFirst + rel, chunk, src, true) != kIOReturnSuccess) return 0;
    src = static_cast<const uint8_t *> (src) + chunk * 4096ULL;
    rel += chunk;
    blocks -= chunk;
  }
  return 1;
}

extern "C" int native_ufs_store_sync (void)
{
  return storeMode () == 2 && gStoreRegionBlocks && hostSyncCache () == kIOReturnSuccess;
}

// ---- Diagnostic log window (miro-log=1) ---------------------------------------------------------------------------
// A board log writer gets the last kLogReserveBlocks of the root partition and the kExportBlocks below them, refused
// unless the partition holds them clear of the flashed root image plus a 4 MiB guard. Off without the boot argument.
static unsigned logMode (void)
{
  static int cached = -1;
  if (cached < 0) {
    uint32_t value = 0;
    if (!PE_parse_boot_argn ("miro-log", &value, sizeof (value)) || value > 1) value = 0;
    cached = static_cast<int> (value);
  }
  return static_cast<unsigned> (cached);
}

// Returns 1 with the window's first absolute LBA and size in 4096-byte blocks, 0 on refusal (reason in *why, may be null).
extern "C" int native_ufs_log_open (uint64_t *firstLba, uint64_t *blocks, const char **why)
{
  const char *reason = "";
  int ok = 0;
  if (!logMode ()) reason = "off";
  else if (!gStoreGeometry.valid || !gHost.ready) reason = "no root layout";
  else if (gLogRegionBlocks) reason = "already open";
  else {
    uint64_t partition = gStoreGeometry.last - gStoreGeometry.first + 1;
    uint64_t imageBlocks = (gStoreGeometry.imageBytes + 4095) / 4096;
    if (reserveBlocks () < kLogReserveBlocks + kExportBlocks) reason = "reserve too small for the log";
    else if (partition - reserveBlocks () < imageBlocks + 1024) reason = "overlaps the root image";
    else {
      uint64_t first = gStoreGeometry.last + 1 - kLogReserveBlocks;
      // One write window covers the export region and the log: [first - kExportBlocks, first + kLogReserveBlocks).
      if (!hostLogOpen (first - kExportBlocks, kExportBlocks + kLogReserveBlocks)) reason = "window refused";
      else {
        gLogRegionFirst = first;
        gLogRegionBlocks = kLogReserveBlocks;
        if (firstLba) *firstLba = first;
        if (blocks) *blocks = kLogReserveBlocks;
        ok = 1;
      }
    }
  }
  if (why) *why = reason;
  return ok;
}

extern "C" int native_ufs_log_write (uint64_t rel, uint32_t blocks, const void *src)
{
  if (!gLogRegionBlocks || !src || !blocks || rel >= gLogRegionBlocks || blocks > gLogRegionBlocks - rel) return 0;
  while (blocks) {
    uint32_t chunk = blocks > kDataMax / 4096 ? static_cast<uint32_t> (kDataMax / 4096) : blocks;
    if (hostWrite4k (0, gLogRegionFirst + rel, chunk, src) != kIOReturnSuccess) return 0;
    src = static_cast<const uint8_t *> (src) + chunk * 4096ULL;
    rel += chunk;
    blocks -= chunk;
  }
  return 1;
}

// Bring-up probe: one diagnostic block in the crash-log partition, next to the panic record and independent of the log
// window. It proves the writer reached the medium even when the log itself is off.
extern "C" int native_ufs_log_probe (uint32_t status, const char *why)
{
  if (!gHost.ready) return 0;
  const char *reason = "";
  uint64_t window = 0;
  if (!panicWindowOpen (&window, &reason)) return 0;
  const uint64_t last = window + kPanicWindowBlocks - 1;
  static uint8_t block[4096];
  for (unsigned i = 0; i < sizeof (block); ++i) block[i] = 0;
  const char magic[8] = {'D', 'A', 'D', 'A', 'P', 'R', 'B', '1'};
  for (unsigned i = 0; i < 8; ++i) block[i] = magic[i];
  block[8] = uint8_t (status); block[9] = uint8_t (status >> 8);
  block[10] = uint8_t (status >> 16); block[11] = uint8_t (status >> 24);
  block[12] = gStoreGeometry.valid ? 1 : 0;
  block[13] = gLogRegionBlocks ? 1 : 0;
  block[14] = gHost.ready ? 1 : 0;
  for (unsigned i = 0; i < 8; ++i) block[16 + i] = uint8_t (last >> (8 * i));
  if (why) { unsigned i = 0; while (i < 64 && why[i]) { block[24 + i] = uint8_t (why[i]); ++i; } }
  return hostWrite4k (0, window, 1, block) == kIOReturnSuccess;
}

// Panic-time record: "MIROPNC1" + version + byte count + CRC32 + text in the last blocks of the board's crash-log
// partition (kPanicPartition). It needs neither a manifest nor the log window, so a boot that dies before root publication
// still leaves its message on the medium; scripts/read-miro-log.py scans that partition for the magic.
//
// The target is deliberately NOT the root partition. On this board the root partition is the Android userdata partition, and
// its tail holds live filesystem data: on 2026-10-29 a panic record aimed there sat inside the window a 9.38 GiB root image
// had just destroyed, and no rollback existed. A partition whose only contents are previous kernel oops records costs
// nothing to overwrite, is not encrypted (so it reads back without root), and survives the next Android boot.
extern "C" int native_ufs_panic_write (const char *text, unsigned long length)
{
  static uint8_t record[kPanicRecordBlocks * 4096];
  if (!text || !length || !gHost.ready) return 0;
  const char *reason = "";
  uint64_t window = 0;
  if (!panicWindowOpen (&window, &reason)) return 0;
  if (length > sizeof (record) - 32) length = sizeof (record) - 32;
  for (unsigned i = 0; i < sizeof (record); ++i) record[i] = 0;
  const char magic[8] = {'D', 'A', 'D', 'A', 'P', 'N', 'C', '1'};
  for (unsigned i = 0; i < 8; ++i) record[i] = uint8_t (magic[i]);
  uint32_t version = 1, bytes = static_cast<uint32_t> (length), crc = rootCrc32 (reinterpret_cast<const uint8_t *> (text), length), blocks = kPanicRecordBlocks;
  memcpy (record + 8, &version, 4);
  memcpy (record + 12, &bytes, 4);
  memcpy (record + 16, &crc, 4);
  memcpy (record + 20, &blocks, 4);
  memcpy (record + 32, text, length);
  if (hostWrite4k (0, window + kProbeBlocks, kPanicRecordBlocks, record, true) != kIOReturnSuccess) return 0;
  hostSyncCache ();
  return 1;
}

// Read the newest rounds back out of the ring l23 writes. Used by the kernel's
// MIRO-DIGEST so a boot can see the previous boot's errors without EDL.
// Copies up to `room` bytes into dst, oldest of the selected rounds first, and
// returns the byte count (0 when the ring holds nothing valid).
extern "C" int native_ufs_oops_log_read (char *dst, uint32_t room, uint32_t *roundsOut)
{
  if (!dst || !room) return 0;
  const char *why = "";
  uint64_t first = 0;
  if (!oopsLogWindow (&first, &why)) return 0;
  static uint8_t header[4096];
  const uint64_t slots = kOopsLogBlocks / kOopsSlotBlocks;
  // Collect the valid rounds, newest first, then emit oldest-first.
  struct Round { uint32_t sequence, length, slot; };
  static Round found[kOopsLogBlocks / kOopsSlotBlocks];
  uint32_t count = 0, highest = 0;
  for (uint64_t slot = 0; slot < slots; ++slot) {
    if (hostRead4k (0, first + slot * kOopsSlotBlocks, 1, header) != kIOReturnSuccess) continue;
    if (memcmp (header, "OOPSLOG1", 8) != 0) continue;
    uint32_t sequence = 0, length = 0, slotField = 0;
    memcpy (&sequence, header + 8, 4);
    memcpy (&length, header + 12, 4);
    memcpy (&slotField, header + 16, 4);
    if (length > (kOopsSlotBlocks - 1) * 4096) continue;
    if (count < slots) {
      found[count].sequence = sequence;
      found[count].length = length;
      found[count].slot = static_cast<uint32_t> (slot);
      ++count;
      if (sequence > highest) highest = sequence;
    }
  }
  if (!count) return 0;
  if (roundsOut) *roundsOut = count;
  // Walk backwards from `highest` so the newest round is copied last and stays visible.
  uint32_t written = 0;
  for (uint32_t want = highest; want > 0 && want + slots > highest; --want) {
    const Round *pick = nullptr;
    for (uint32_t i = 0; i < count; ++i) {
      if (found[i].sequence == want) { pick = &found[i]; break; }
    }
    if (!pick) continue;
    uint32_t remaining = pick->length;
    if (remaining > room - written) break;      // no partial rounds
    uint32_t done = 0;
    while (done < remaining) {
      uint32_t chunk = remaining - done;
      if (chunk > 4096) chunk = 4096;
      static uint8_t body[4096];
      if (hostRead4k (0, first + pick->slot * kOopsSlotBlocks + 1 + done / 4096, 1, body) != kIOReturnSuccess) return written;
      memcpy (dst + written + done, body, chunk);
      done += chunk;
    }
    written += remaining;
  }
  return static_cast<int> (written);
}

// Timed return to the bootloader (miro-autoreset=<seconds>).
// PSCI SYSTEM_RESET over SMCCC, so a RAM-booted image comes back to fastboot on its own. Every other reset path on this
// board needs the power button: PE_halt_restart is never assigned on arm64 and there is no PSCI plumbing in pexpert.
static uint64_t gAutoResetTicks = 0;
static uint64_t gAutoResetStart = 0;

extern "C" void native_ufs_autoreset_arm (uint32_t seconds)
{
  if (!seconds) return;
  uint64_t freq = 0;
  __asm__ volatile ("mrs %0, cntfrq_el0" : "=r" (freq));
  if (!freq) freq = 19200000;
  uint64_t now = 0;
  __asm__ volatile ("mrs %0, cntpct_el0" : "=r" (now));
  gAutoResetStart = now;
  gAutoResetTicks = static_cast<uint64_t> (seconds) * freq;
}

// Called from the log tick. Returns 1 when the reset has been asked for (it does not return in practice).
static int native_ufs_autoreset_check (void)
{
  if (!gAutoResetTicks) return 0;
  uint64_t now = 0;
  __asm__ volatile ("mrs %0, cntpct_el0" : "=r" (now));
  if (now - gAutoResetStart < gAutoResetTicks) return 0;
  gAutoResetTicks = 0;
  // SMCCC PSCI: function id SYSTEM_RESET = 0x84000009, no arguments.
  register uint64_t x0 __asm__ ("x0") = 0x84000009ULL;
  register uint64_t x1 __asm__ ("x1") = 0;
  register uint64_t x2 __asm__ ("x2") = 0;
  register uint64_t x3 __asm__ ("x3") = 0;
  __asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3) :: "memory");
  return 1;
}

extern "C" int native_ufs_autoreset_poll (void) { return native_ufs_autoreset_check (); }

// The export region below the log window: its size in blocks once the log is open, else 0; writes at a block offset inside it.
extern "C" uint64_t native_ufs_export_blocks (void) { return gLogRegionBlocks ? kExportBlocks : 0; }
// Read back the previous boot's crash record (miro l16). The write side is native_ufs_panic_write; this is its reader, so a
// record can be recovered without root, TWRP or a host-side partition dump: the kernel prints it at the next boot.
// Returns the text length, 0 when there is no valid record, -1 when the partition or the window is unavailable.
extern "C" int native_ufs_panic_read (char *dst, unsigned long room, unsigned long *crcState)
{
  if (!dst || room < 2 || !gHost.ready) return -1;
  const char *reason = "";
  uint64_t window = 0;
  if (!panicWindowOpen (&window, &reason)) return -1;
  static uint8_t record[kPanicRecordBlocks * 4096];
  if (hostRead4k (0, window + kProbeBlocks, kPanicRecordBlocks, record) != kIOReturnSuccess) return -1;
  const char magic[8] = {'D', 'A', 'D', 'A', 'P', 'N', 'C', '1'};
  for (unsigned i = 0; i < 8; ++i) {
    if (record[i] != uint8_t (magic[i])) return 0;
  }
  uint32_t version = 0, bytes = 0, crc = 0;
  memcpy (&version, record + 8, 4);
  memcpy (&bytes, record + 12, 4);
  memcpy (&crc, record + 16, 4);
  if (version != 1 || !bytes || bytes > sizeof (record) - 32) return 0;
  const uint8_t *body = record + 32;
  uint32_t actual = rootCrc32 (body, bytes);
  if (crcState) *crcState = (crc == actual) ? 0 : 1;
  unsigned long copy = bytes < room - 1 ? bytes : room - 1;
  memcpy (dst, body, copy);
  dst[copy] = 0;
  return static_cast<int> (copy);
}


extern "C" int native_ufs_export_write (uint64_t rel, uint32_t blocks, const void *src)
{
  if (!gLogRegionBlocks || !src || !blocks || rel >= kExportBlocks || blocks > kExportBlocks - rel) return 0;
  const uint64_t base = gLogRegionFirst - kExportBlocks;
  while (blocks) {
    uint32_t chunk = blocks > kDataMax / 4096 ? static_cast<uint32_t> (kDataMax / 4096) : blocks;
    if (hostWrite4k (0, base + rel, chunk, src) != kIOReturnSuccess) return 0;
    src = static_cast<const uint8_t *> (src) + chunk * 4096ULL;
    rel += chunk;
    blocks -= chunk;
  }
  return 1;
}

extern "C" int native_ufs_log_sync (void)
{
  return gLogRegionBlocks && hostSyncCache () == kIOReturnSuccess;
}

// Re-read the GPT header and entry array and compare their CRCs with what was recorded at root start. 0 = unchanged.
extern "C" unsigned native_ufs_store_gpt_check (void)
{
  if (!gStoreGeometry.valid || !gHost.ready) return 1;
  static uint8_t block[4096];
  if (hostRead4k (0, 1, 1, block) != kIOReturnSuccess) return 2;
  uint32_t headerBytes = le32 (block + 12);
  if (headerBytes < 92 || headerBytes > 4096 || rootCrc32 (block, headerBytes, true) != le32 (block + 16) ||
      le32 (block + 16) != gStoreGeometry.headerCrc) return 3;
  uint32_t blocks = (gStoreGeometry.entryCount * 128 + 4095) / 4096;
  static uint8_t entries[4096 * 4];
  if (blocks > 4 || hostRead4k (0, gStoreGeometry.entriesLba, blocks, entries) != kIOReturnSuccess) return 4;
  return rootCrc32 (entries, gStoreGeometry.entryCount * 128) == gStoreGeometry.entriesCrc ? 0 : 5;
}

#ifdef NATIVE_UFS_BLOCK_DEVICE
// One read-only window of the root image as a block device: 512-byte sectors on top of 4096-byte
// UFS blocks, so the unchanged dmg disk images (GPT with 512-byte sectors) parse as they do on
// the virtio path.
class NativeUfsBlock : public IOBlockStorageDevice {
  OSDeclareDefaultStructors (NativeUfsBlock)
  uint64_t absolute = 0;      // byte offset of the window on LUN 0
  uint64_t bytes = 0;
  char label[32] = {};
  uint8_t *bounce = nullptr;
  IOLock *windowLock = nullptr;
  bool online = false;
#if defined(NATIVE_VERIFIED_BLOCK) && NATIVE_VERIFIED_BLOCK
  struct vr_view verified = {};
  vm_offset_t verifiedStorage = 0;
  vm_size_t verifiedStorageBytes = 0;
  bool configureVerification ();
#endif
public:
#if defined(NATIVE_VERIFIED_BLOCK) && NATIVE_VERIFIED_BLOCK
  bool authenticatesVolume (const char *uuid, uint8_t *manifestDigest = nullptr) const;
  bool signedVolumeUuid (char out[37]) const;
#endif
  const char *windowLabel () const { return label; }
  bool configure (const char *name, uint64_t absoluteOffset, uint64_t size);
  bool start (IOService *provider) override;
  void free () override;
  IOReturn doEjectMedia () override { return kIOReturnUnsupported; }
  IOReturn doFormatMedia (UInt64) override { return kIOReturnNotWritable; }
  UInt32 doGetFormatCapacities (UInt64 *, UInt32) const override { return 0; }
  char *getVendorString () override { return const_cast<char *> ("Native"); }
  char *getProductString () override { return label; }
  char *getRevisionString () override { return const_cast<char *> ("1.0"); }
  char *getAdditionalDeviceInfoString () override { return const_cast<char *> ("UFS LUN 0 root window, read-only"); }
  IOReturn reportBlockSize (UInt64 *value) override { *value = 512; return kIOReturnSuccess; }
  IOReturn reportEjectability (bool *value) override { *value = false; return kIOReturnSuccess; }
  IOReturn reportMaxValidBlock (UInt64 *value) override { *value = bytes / 512 - 1; return kIOReturnSuccess; }
  IOReturn reportMediaState (bool *present, bool *changed) override {
    *present = online; if (changed) *changed = false; return kIOReturnSuccess;
  }
  IOReturn reportRemovability (bool *value) override { *value = false; return kIOReturnSuccess; }
  IOReturn reportWriteProtection (bool *value) override { *value = true; return kIOReturnSuccess; }
  IOReturn doSynchronizeCache () override { return online ? kIOReturnSuccess : kIOReturnNotReady; }
  IOReturn doSynchronize (UInt64, UInt64, IOOptionBits) override { return doSynchronizeCache (); }
  IOReturn getWriteCacheState (bool *value) override { *value = false; return kIOReturnSuccess; }
  IOReturn setWriteCacheState (bool) override { return kIOReturnUnsupported; }
  IOReturn doAsyncReadWrite (IOMemoryDescriptor *, UInt64, UInt64, IOStorageAttributes *, IOStorageCompletion *) override;
};
OSDefineMetaClassAndStructors (NativeUfsBlock, IOBlockStorageDevice)
#endif // NATIVE_UFS_BLOCK_DEVICE

extern "C" {
volatile uint32_t native_ufs_blocks_published = 0;
volatile uint64_t native_ufs_block_reads = 0;
volatile uint64_t native_ufs_block_failures = 0;
}

static char gRootDiagnosticText[2500];
static char gRootMediaText[1000];
// Passive diagnostic reader: cached software values only. It must remain safe
// when called from an IRQ while the transport is busy: no locks, allocations,
// MMIO reads, queue resets, or changes to the command/trust state. Fields can
// represent adjacent updates; this is explicitly not an atomic HW snapshot.
extern "C" void native_ufs_progress_text (char *out, size_t size)
{
  UfsRootDiagnostics sample = gRootDiagnostic;
  sample.published = native_ufs_blocks_published;
  sample.reads = native_ufs_block_reads;
  sample.failures = native_ufs_block_failures;
  formatUfsRootDiagnostics (out, size, sample, "cached values; not a new HW fault sample", "UFS CACHED SNAPSHOT");
}

static void rootDiagnosticFailure (const char *reason)
{
  // Kept even when the panic diagnostic is off, so the boot page can say why nothing was published.
  native_ufs_root_failure = reason;
  if (!gRootDiagnosticEnabled) return;
  gRootDiagnostic.published = native_ufs_blocks_published;
  gRootDiagnostic.reads = native_ufs_block_reads;
  gRootDiagnostic.failures = native_ufs_block_failures;
  formatUfsRootDiagnostics (gRootDiagnosticText, sizeof (gRootDiagnosticText), gRootDiagnostic, reason);
  strlcat (gRootDiagnosticText, gRootMediaText, sizeof (gRootDiagnosticText));
  panic ("%s", gRootDiagnosticText);
}

#ifdef NATIVE_UFS_BLOCK_DEVICE
bool NativeUfsBlock::configure (const char *name, uint64_t absoluteOffset, uint64_t size)
{
  if (!name || !ufsWindowGeometry (absoluteOffset, size)) {
    return false;
  }
  strlcpy (label, name, sizeof (label));
  absolute = absoluteOffset;
  bytes = size;
  windowLock = IOLockAlloc ();
  if (!windowLock) return false;
  bounce = static_cast<uint8_t *> (IOMalloc (kDataMax));
  if (!bounce) {
    return false;
  }
#if defined(NATIVE_VERIFIED_BLOCK) && NATIVE_VERIFIED_BLOCK
  // Reads are served only after each 4096-byte integrity block verifies against the owner-signed
  // policy; without a matching policy the device is never published.
  return configureVerification ();
#else
  return true;
#endif
}

// The read timer is not part of verified block mode: every transport read is timed, whether or not the
// window is authenticated. Both symbols therefore stay outside the guard below -- the read path uses them
// unconditionally, and keeping them inside made a plain (unverified) NativeUfs build fail with
// "use of undeclared identifier 'timerTicks'".
volatile uint64_t native_ufs_read_ticks = 0;
static inline uint64_t timerTicks (void) {uint64_t v; __asm__ volatile ("isb\n\tmrs %0, cntpct_el0" : "=r" (v)); return v;}

#if defined(NATIVE_VERIFIED_BLOCK) && NATIVE_VERIFIED_BLOCK
volatile uint64_t native_ufs_verified_blocks = 0;
// Time spent hashing/verifying, in generic-timer ticks (diagnostic only).
volatile uint64_t native_ufs_verify_ticks = 0;
volatile uint32_t native_ufs_integrity_failures = 0;

bool NativeUfsBlock::configureVerification ()
{
  gRootDiagnostic.stage = "policy selection";
  gRootDiagnostic.windowBytes = bytes;
  gRootDiagnostic.policyCount = verified_root_policy_count;
  gRootDiagnostic.verify = 0xffffffff;
  // Select the one policy whose signed image size equals this window; two matches are refused.
  const uint8_t *manifest = nullptr;
  uint64_t manifestBytes = 0;
  if (!verified_root_policy_count || verified_root_policy_count > 16) {
    gRootDiagnostic.stage = "policy count invalid";
    return false;
  }
  for (uint64_t index = 0; index < verified_root_policy_count; ++index) {
    const NativeVerifiedBlockPolicy &policy = verified_root_policies[index];
    if (!policy.manifest || policy.manifestBytes < VR_HEADER_BYTES + VR_SIGNATURE_BYTES) {
      gRootDiagnostic.stage = "policy envelope invalid";
      return false;
    }
    uint64_t imageBytes = 0;
    for (unsigned i = 0; i < 8; ++i) {
      imageBytes |= static_cast<uint64_t> (policy.manifest[24 + i]) << (8 * i);
    }
    if (imageBytes != bytes) {
      continue;
    }
    if (manifest) {
      gRootDiagnostic.stage = "duplicate capacity-matching policy";
      return false;
    }
    manifest = policy.manifest;
    manifestBytes = policy.manifestBytes;
  }
  if (!manifest) {
    gRootDiagnostic.stage = "no capacity-matching policy";
    return false;
  }
  gRootDiagnostic.stage = "policy storage";
  vm_size_t tableBytes = round_page (static_cast<vm_size_t> (manifestBytes));
  if (tableBytes < manifestBytes || tableBytes > UINT64_MAX - 4096) {
    gRootDiagnostic.stage = "policy size overflow";
    return false;
  }
  verifiedStorageBytes = tableBytes + 4096;
  gRootDiagnostic.stage = "policy allocation";
  if (kmem_alloc (kernel_map, &verifiedStorage, verifiedStorageBytes,
                  static_cast<kma_flags_t> (KMA_KOBJECT | KMA_DATA), VM_KERN_MEMORY_SECURITY) != KERN_SUCCESS) {
    return false;
  }
  memcpy (reinterpret_cast<void *> (verifiedStorage), manifest, manifestBytes);
  uint8_t *key = reinterpret_cast<uint8_t *> (verifiedStorage + tableBytes);
  memcpy (key, verified_root_key, VR_KEY_BYTES);
  gRootDiagnostic.stage = "policy read-only protection";
  if (vm_map_protect (kernel_map, verifiedStorage, verifiedStorage + verifiedStorageBytes,
                      VM_PROT_READ, TRUE) != KERN_SUCCESS) {
    return false;
  }
  gRootDiagnostic.stage = "owner signature and hash table";
  int status = vr_open (reinterpret_cast<const uint8_t *> (verifiedStorage), manifestBytes, key, 1,
                        &vr_xnu_crypto, &verified);
  gRootDiagnostic.verify = static_cast<uint32_t> (status);
  return status == VR_OK && verified.image_bytes == bytes;
}

bool NativeUfsBlock::authenticatesVolume (const char *uuid, uint8_t *manifestDigest) const
{
  if (!uuid || !online || !verifiedStorage || !verified.hashes) {
    return false;
  }
  static const char hex[] = "0123456789ABCDEF";
  char expected[37];
  unsigned at = 0;
  for (unsigned i = 0; i < 16; ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) {
      expected[at++] = '-';
    }
    expected[at++] = hex[verified.volume_uuid[i] >> 4];
    expected[at++] = hex[verified.volume_uuid[i] & 15];
  }
  expected[at] = 0;
  if (strcmp (uuid, expected)) {
    return false;
  }
  if (manifestDigest) {
    // Hash only the authenticated envelope, excluding allocation padding and the key.
    const uint64_t envelope = VR_HEADER_BYTES + VR_SIGNATURE_BYTES + verified.block_count * 32;
    SHA384_CTX context;
    SHA384_Init (&context);
    SHA384_Update (&context, reinterpret_cast<const void *> (verifiedStorage), envelope);
    SHA384_Final (manifestDigest, &context);
  }
  return true;
}

bool NativeUfsBlock::signedVolumeUuid (char out[37]) const
{
  if (!verifiedStorage || !verified.hashes) {
    return false;
  }
  static const char hex[] = "0123456789ABCDEF";
  unsigned at = 0;
  for (unsigned i = 0; i < 16; ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) {
      out[at++] = '-';
    }
    out[at++] = hex[verified.volume_uuid[i] >> 4];
    out[at++] = hex[verified.volume_uuid[i] & 15];
  }
  out[at] = 0;
  return true;
}

// Called from the root authorization query in NativeVirtioBlock: -1 when `provider` is not a
// UFS window, otherwise 1 if its signed volume UUID equals `uuid`, else 0.
extern "C" int native_ufs_block_authenticates (IOService *provider, const char *uuid, uint8_t *digest)
{
  NativeUfsBlock *block = OSDynamicCast (NativeUfsBlock, provider);
  if (!block) {
    return -1;
  }
  return block->authenticatesVolume (uuid, digest) ? 1 : 0;
}
#endif

bool NativeUfsBlock::start (IOService *provider)
{
  if (!bounce || !windowLock || !IOBlockStorageDevice::start (provider)) {
    return false;
  }
  online = true;
  registerService ();
  return true;
}

IOReturn NativeUfsBlock::doAsyncReadWrite (IOMemoryDescriptor *buffer, UInt64 block, UInt64 count,
                                           IOStorageAttributes *, IOStorageCompletion *completion)
{
  if (!buffer || !completion || !completion->action || !count || block >= bytes / 512 ||
      count > bytes / 512 - block || buffer->getLength () < count * 512) {
    return kIOReturnBadArgument;
  }
  if (buffer->getDirection () != kIODirectionIn) {
    return kIOReturnNotWritable;
  }
  if (!online) {
    return kIOReturnNotReady;
  }
  struct Operations {
    IOLock *mutex;
    IOMemoryDescriptor *buffer;
#if defined(NATIVE_VERIFIED_BLOCK) && NATIVE_VERIFIED_BLOCK
    const vr_view *view;
#endif
    void lock () { IOLockLock (mutex); }
    void unlock () { IOLockUnlock (mutex); }
    IOReturn read (uint64_t lba, uint32_t blocks, void *out) {
      uint64_t t0 = timerTicks ();
      IOReturn r = hostRead4k (0, lba, blocks, out);
      __atomic_fetch_add (&native_ufs_read_ticks, timerTicks () - t0, __ATOMIC_RELAXED);
      return r;
    }
    IOReturn verify (uint64_t index, const void *data, size_t length) {
#if defined(NATIVE_VERIFIED_BLOCK) && NATIVE_VERIFIED_BLOCK
      uint64_t t0 = timerTicks ();
      int status = vr_verify_block (view, index, data, length, &vr_xnu_crypto);
      __atomic_fetch_add (&native_ufs_verify_ticks, timerTicks () - t0, __ATOMIC_RELAXED);
      if (status != VR_OK) {
        gRootDiagnostic.stage = "read block integrity";
        gRootDiagnostic.verify = static_cast<uint32_t> (status);
        __atomic_fetch_add (&native_ufs_integrity_failures, 1, __ATOMIC_RELAXED);
        return kIOReturnNotPrivileged;
      }
      __atomic_fetch_add (&native_ufs_verified_blocks, 1, __ATOMIC_RELAXED);
      if (boot_progress_poll) boot_progress_poll ();
#else
      (void) index; (void) data; (void) length;
#endif
      return kIOReturnSuccess;
    }
    uint64_t copy (uint64_t offset, const void *data, uint64_t length) {
      uint64_t copied = buffer->writeBytes (offset, data, length);
      if (copied == length) __atomic_fetch_add (&native_ufs_block_reads, 1, __ATOMIC_RELAXED);
      return copied;
    }
  } operations = { windowLock, buffer
#if defined(NATIVE_VERIFIED_BLOCK) && NATIVE_VERIFIED_BLOCK
    , &verified
#endif
  };
  UfsWindowResult outcome = ufsReadWindow (operations, absolute, bytes, block, count, bounce);
  IOReturn result = outcome.status;
  uint64_t done = outcome.done;
  if (result != kIOReturnSuccess) {
    __atomic_fetch_add (&native_ufs_block_failures, 1, __ATOMIC_RELAXED);
    gRootDiagnostic.result = static_cast<uint32_t> (result);
    rootDiagnosticFailure ("root window read failed");
  }
  completion->action (completion->target, completion->parameter, result, done);
  return kIOReturnSuccess;
}

void NativeUfsBlock::free ()
{
#if defined(NATIVE_VERIFIED_BLOCK) && NATIVE_VERIFIED_BLOCK
  if (verifiedStorage) {
    kmem_free (kernel_map, verifiedStorage, verifiedStorageBytes);
    verifiedStorage = 0;
  }
#endif
  if (bounce) {
    IOFree (bounce, kDataMax);
    bounce = nullptr;
  }
  if (windowLock) {
    IOLockFree (windowLock);
    windowLock = nullptr;
  }
  IOBlockStorageDevice::free ();
}

// Ordered publication of the root windows. The original owner-trust code resolves the SystemOS
// volume by UUID immediately after the root is mounted, so SystemOS must already have published its
// volume media by then; the OS window (whose volume is the root being waited for) goes last.
static NativeUfsBlock *gPendingDevice[4];
static unsigned gPending;
static bool gPendingAttached[4];
static char gPendingUuid[4][37];
static UfsRootPlan gPublicationPlan;
static NativeUfs *gPendingProvider;

#if defined(NATIVE_VERIFIED_BLOCK) && NATIVE_VERIFIED_BLOCK
static bool mediaWithUuidExists (const char *uuid, NativeUfsBlock *window)
{
  OSDictionary *matching = IOService::serviceMatching ("IOMedia");
  OSDictionary *properties = OSDictionary::withCapacity (1);
  OSString *wanted = OSString::withCString (uuid);
  bool found = false;
  if (matching && properties && wanted && properties->setObject ("UUID", wanted) &&
      matching->setObject ("IOPropertyMatch", properties)) {
    OSIterator *iterator = IOService::getMatchingServices (matching);
    if (iterator) {
      for (unsigned matches = 0; matches < 64 && !found; ++matches) {
        IORegistryEntry *media = OSDynamicCast (IORegistryEntry, iterator->getNextObject ());
        if (!media) break;
        IORegistryIterator *parents = IORegistryIterator::iterateOver (
          media, gIOServicePlane, kIORegistryIterateRecursively | kIORegistryIterateParents);
        if (parents) {
          found = ufsMediaHasAncestor (*parents, window);
          parents->release ();
        }
      }
      iterator->release ();
    }
  }
  OSSafeReleaseNULL (wanted);
  OSSafeReleaseNULL (properties);
  OSSafeReleaseNULL (matching);
  return found;
}
#endif // NATIVE_VERIFIED_BLOCK

// Ready when any medium's provider chain reaches this window, ignoring the UUID. This is the unsigned
// publication's readiness test: the UUIDs the verified path matches on come from signed policies, and a
// build without owner trust has none. The window's own IOMedia is proof that IOBlockStorageDriver,
// IOGUIDPartitionScheme and APFS have picked the device up.
#if !defined(NATIVE_VERIFIED_BLOCK) || !NATIVE_VERIFIED_BLOCK
static bool mediaUnderWindow (NativeUfsBlock *window)
{
  bool found = false;
  OSDictionary *matching = IOService::serviceMatching ("IOMedia");
  if (matching) {
    OSIterator *iterator = IOService::getMatchingServices (matching);
    if (iterator) {
      for (unsigned seen = 0; seen < 64 && !found; ++seen) {
        IORegistryEntry *media = OSDynamicCast (IORegistryEntry, iterator->getNextObject ());
        if (!media) break;
        IORegistryIterator *parents = IORegistryIterator::iterateOver (
          media, gIOServicePlane, kIORegistryIterateRecursively | kIORegistryIterateParents);
        if (parents) {
          found = ufsMediaHasAncestor (*parents, window);
          parents->release ();
        }
      }
      iterator->release ();
    }
    matching->release ();
  }
  return found;
}
#endif // !NATIVE_VERIFIED_BLOCK

static void captureMissingMedia (const char *uuid)
{
  int used = snprintf (gRootMediaText, sizeof (gRootMediaText), uuid[0] ? "Waiting for signed UUID %s\n" : "Media not ready\n", uuid);
  OSDictionary *matching = IOService::serviceMatching ("IOMedia");
  OSIterator *iterator = matching ? IOService::getMatchingServices (matching) : nullptr;
  unsigned count = 0;
  while (iterator && used < int (sizeof (gRootMediaText)) - 180 && count++ < 8) {
    IOService *media = OSDynamicCast (IOService, iterator->getNextObject ());
    if (!media) break;
    OSString *id = OSDynamicCast (OSString, media->getProperty ("UUID"));
    OSString *bsd = OSDynamicCast (OSString, media->getProperty (kIOBSDNameKey));
    OSNumber *size = OSDynamicCast (OSNumber, media->getProperty ("Size"));
    used += snprintf (gRootMediaText + used, sizeof (gRootMediaText) - used,
                      "media %.24s bytes=%llx uuid=%.36s\n",
                      bsd ? bsd->getCStringNoCopy () : "-", size ? size->unsigned64BitValue () : 0ULL,
                      id ? id->getCStringNoCopy () : "-");
  }
  OSSafeReleaseNULL (iterator);
  OSSafeReleaseNULL (matching);
}

#if !defined(NATIVE_VERIFIED_BLOCK) || !NATIVE_VERIFIED_BLOCK
// Offline owner cache mount (XNU_OWNER_CACHE_MOUNT) needs the transport to name the SystemOS volume
// device and to state whether a device is authorized. This build carries no signature policy, so the
// bring-up verdict authorizes the published windows and the lookup matches the IOMedia by the APFS
// volume UUID the kernel passes (8846FE78-... in the selected image). The kernel then mounts that
// volume and hands launchd DYLD_SHARED_CACHE_DIR pointing at its dyld cache.
extern "C" int native_verified_block_root_authorized (dev_t device)
{
  return device != NODEV;
}

static int cacheMediaCandidate (IOService *media, const char *uuid, bool by_size,
    dev_t *device, char *path, size_t size)
{
  if (by_size) {
    OSNumber *bytes = OSDynamicCast (OSNumber, media->getProperty ("Size"));
    if (!bytes || bytes->unsigned64BitValue () != 3303014400ULL) return 0;
  } else {
    OSString *identity = OSDynamicCast (OSString, media->getProperty ("UUID"));
    if (!identity || !uuid || strcmp (identity->getCStringNoCopy (), uuid)) return 0;
  }
  OSString *name = OSDynamicCast (OSString, media->getProperty (kIOBSDNameKey));
  if (!name || !name->getLength () || name->getLength () >= size - 6 ||
      name->getLength () >= 56) return 0;
  char candidatePath[64];
  snprintf (candidatePath, sizeof (candidatePath), "/dev/%s", name->getCStringNoCopy ());
  dev_t candidate = NODEV;
  vnode_t vp = NULLVP;
  if (vnode_lookup (candidatePath, 0, &vp, vfs_context_current ()) == 0) {
    candidate = vnode_specrdev (vp);
    vnode_put (vp);
  }
  if (candidate == NODEV) {
    OSNumber *major = OSDynamicCast (OSNumber, media->getProperty (kIOBSDMajorKey));
    OSNumber *minor = OSDynamicCast (OSNumber, media->getProperty (kIOBSDMinorKey));
    if (major && minor && major->unsigned64BitValue () <= 255 && minor->unsigned64BitValue () <= 0xffffff) {
      candidate = static_cast<dev_t> (makedev (major->unsigned32BitValue (), minor->unsigned32BitValue ()));
    }
  }
  if (candidate == NODEV) return 0;
  *device = candidate;
  strlcpy (path, candidatePath, size);
  return 1;
}

extern "C" int native_verified_block_volume_device (const char *uuid, dev_t *device, char *path, size_t size)
{
  if (!device || !path || size < 6) return 0;
  *device = NODEV;
  path[0] = 0;
  for (unsigned pass = 0; pass < 2; pass++) {
    if (pass == 0 && (!uuid || strlen (uuid) != 36)) continue;
    OSDictionary *matching = IOService::serviceMatching ("IOMedia");
    OSDictionary *properties = OSDictionary::withCapacity (1);
    bool ready = false;
    if (matching && properties) {
      if (pass == 0) {
        OSString *wanted = OSString::withCString (uuid);
        ready = wanted && properties->setObject ("UUID", wanted) &&
                matching->setObject ("IOPropertyMatch", properties);
        OSSafeReleaseNULL (wanted);
      } else {
        OSNumber *wantedSize = OSNumber::withNumber (3303014400ULL, 64);
        ready = wantedSize && properties->setObject ("Size", wantedSize) &&
                matching->setObject ("IOPropertyMatch", properties);
        OSSafeReleaseNULL (wantedSize);
      }
    }
    OSSafeReleaseNULL (properties);
    OSIterator *iterator = ready ? IOService::getMatchingServices (matching) : nullptr;
    if (matching) matching->release ();
    if (!iterator) continue;
    unsigned seen = 0;
    while (OSObject *object = iterator->getNextObject ()) {
      IOService *media = OSDynamicCast (IOService, object);
      if (!media || ++seen > 32) continue;
      if (cacheMediaCandidate (media, uuid, pass == 1, device, path, size)) {
        iterator->release ();
        return 1;
      }
    }
    iterator->release ();
  }
  return 0;
}
#endif

// Used only for objects that were never started/published. Started objects
// retain their owner references even on a later failure; clients can have I/O.
static void discardPendingWindow (unsigned index)
{
  NativeUfsBlock *device = gPendingDevice[index];
  if (!device) return;
  if (gPendingAttached[index]) device->detach (gPendingProvider);
  gPendingAttached[index] = false;
  gPendingDevice[index] = nullptr;
  device->release ();
}

static void publishThread (void *, wait_result_t)
{
  struct Operations {
    bool start (unsigned index) {
      NativeUfsBlock *device = gPendingDevice[index];
      gRootDiagnostic.stage = "root window service start";
      gRootDiagnostic.publicationPhase = static_cast<unsigned> (UfsRootPhase::Start);
      gRootDiagnostic.publicationWindow = index;
      if (!device->start (gPendingProvider)) return false;
      native_ufs_blocks_published = native_ufs_blocks_published + 1;
      IOLog ("NativeUfs: published %s\n", device->windowLabel ());
      return true;
    }
    bool wait (unsigned index) {
      NativeUfsBlock *device = gPendingDevice[index];
      const char *uuid = gPendingUuid[index];
      // 300 x 100 ms is what the verified path waits for a signed volume to appear. APFS needs the same
      // order of magnitude on its own, and the windows are already started at this point.
      const unsigned gateTicks = 300;
      gRootDiagnostic.stage = "media publication";
      gRootDiagnostic.publicationPhase = static_cast<unsigned> (UfsRootPhase::Media);
      gRootDiagnostic.publicationWindow = index;
      bool ready = false;
      // Diagnostic mode affects reporting only, never the dependency gate.
      for (unsigned tick = 0; tick <= gateTicks; ++tick) {
#if defined(NATIVE_VERIFIED_BLOCK) && NATIVE_VERIFIED_BLOCK
        ready = mediaWithUuidExists (uuid, device);
#else
        // No signed policy names a volume UUID, so readiness is the window's own media existing.
        ready = mediaUnderWindow (device);
#endif
        if (ready || tick == gateTicks) break;
        IOSleep (100);
      }
      if (uuid[0]) {
        IOLog ("NativeUfs: %s volume %s %s\n", device->windowLabel (), uuid, ready ? "ready" : "not ready");
      } else {
        IOLog ("NativeUfs: %s media %s\n", device->windowLabel (), ready ? "ready" : "not ready");
      }
      if (!ready && uuid[0]) {
        captureMissingMedia (uuid);
      } else if (!ready) {
        // Unsigned publication: say which media did appear, so a boot log shows how far the stack got.
        gRootMediaText[0] = 0;
        captureMissingMedia (uuid);
      }
      return ready;
    }
    void discard (unsigned index) { discardPendingWindow (index); }
  } operations;
  UfsRootPublicationResult result = ufsPublishRootWindows (operations, gPublicationPlan);
  if (result.phase != UfsRootPhase::Complete) {
    gRootDiagnostic.result = result.phase == UfsRootPhase::Start ? kIOReturnIOError : kIOReturnTimeout;
    // A failed media wait dereferences the window it stopped on, so the label is only read when the
    // window is still there. Start failures have already released theirs.
    const char *label = result.phase == UfsRootPhase::Start ? "" :
      (gPendingDevice[result.index] ? gPendingDevice[result.index]->windowLabel () : "");
    const char *reason = result.phase == UfsRootPhase::Start ? "root window service start failed" :
      (!strcmp (label, "SystemOS") ? "SystemOS volume not found" : "root window volume not found");
    IOLog ("NativeUfs: root publisher stopped at window %u; published=%u\n", result.index, result.published);
    // Start failures have already released their unpublished object; do not
    // dereference it here. The media-failure object is deliberately retained.
    rootDiagnosticFailure (reason);
  }
  gPendingProvider->release ();  // Balanced with successful thread dispatch.
}
#endif // NATIVE_UFS_BLOCK_DEVICE

// Memory snapshot (native-ufs-probe=13): root mode as usual, then, after a delay, the kernel's own
// page counters on the panic screen. Observation only.
extern "C" {
extern unsigned int vm_page_free_count, vm_page_active_count, vm_page_inactive_count,
  vm_page_wire_count, vm_page_speculative_count, vm_page_purgeable_count;
extern uint32_t vm_page_pages;
extern unsigned int memorystatus_available_pages;
extern int nprocs;
}

static void statsThread (void *argument, wait_result_t)
{
  unsigned delay = static_cast<unsigned> (reinterpret_cast<uintptr_t> (argument));
  IOSleep (delay * 1000);
  panic ("NATIVE_UFS memory snapshot after %u s\n"
         "pages total=%u free=%u active=%u inactive=%u wired=%u spec=%u purgeable=%u\n"
         "available=%u max_mem=%llx procs=%d blocks=%llu\n",
         delay, vm_page_pages, vm_page_free_count, vm_page_active_count, vm_page_inactive_count,
         vm_page_wire_count, vm_page_speculative_count, vm_page_purgeable_count,
         memorystatus_available_pages, max_mem, nprocs, native_ufs_block_reads);
}

// Storage discovery report (native-ufs-probe=12): a separate thread waits for the original
// IOStorageFamily to publish IOMedia for the root windows, then shows what exists on the screen.
// It must not run inside the matching thread that is starting NativeUfs.
static char gMediaReport[3000];

static void mediaReportThread (void *, wait_result_t)
{
  unsigned count = 0;
  for (unsigned tick = 0; tick < 200; tick++) {      // up to 20 s
    OSDictionary *matching = IOService::serviceMatching ("IOMedia");
    OSIterator *iterator = matching ? IOService::getMatchingServices (matching) : nullptr;
    count = 0;
    if (iterator) {
      while (iterator->getNextObject ()) {
        count++;
      }
      iterator->release ();
    }
    if (matching) {
      matching->release ();
    }
    if (count >= 4) {
      break;
    }
    IOSleep (100);
  }
  IOSleep (500);
  int used = snprintf (gMediaReport, sizeof (gMediaReport),
                       "NATIVE_UFS media report: published=%u reads=%llu failures=%llu media=%u\n",
                       native_ufs_blocks_published, native_ufs_block_reads, native_ufs_block_failures, count);
  OSDictionary *matching = IOService::serviceMatching ("IOMedia");
  OSIterator *iterator = matching ? IOService::getMatchingServices (matching) : nullptr;
  while (iterator && used < int (sizeof (gMediaReport)) - 200) {
    IOService *media = OSDynamicCast (IOService, iterator->getNextObject ());
    if (!media) {
      break;
    }
    OSString *uuid = OSDynamicCast (OSString, media->getProperty ("UUID"));
    OSString *bsd = OSDynamicCast (OSString, media->getProperty (kIOBSDNameKey));
    OSString *content = OSDynamicCast (OSString, media->getProperty ("Content"));
    OSNumber *size = OSDynamicCast (OSNumber, media->getProperty ("Size"));
    used += snprintf (gMediaReport + used, sizeof (gMediaReport) - used, "%s %s size=%llx uuid=%s\n",
                      bsd ? bsd->getCStringNoCopy () : "-", content ? content->getCStringNoCopy () : "-",
                      size ? size->unsigned64BitValue () : 0ULL, uuid ? uuid->getCStringNoCopy () : "-");
  }
  if (iterator) {
    iterator->release ();
  }
  if (matching) {
    matching->release ();
  }
  panic ("%s", gMediaReport);
}

bool NativeUfs::start (IOService *provider)
{
  unsigned probe = 0, rootMode = 0;
  PE_parse_boot_argn ("native-ufs-probe", &probe, sizeof (probe));
  PE_parse_boot_argn ("native-ufs-root", &rootMode, sizeof (rootMode));
  native_ufs_probe_requested = probe != 0;
  // Default: match for observability only, never touch the controller. Hardware work runs once,
  // from the root-device personality; the ufshc nub match stays observation-only.
  if ((!probe && !rootMode) || !OSDynamicCast (IOPlatformExpertDevice, provider)) {
    return false;
  }
  unsigned diagnostic = 0;
  PE_parse_boot_argn ("native-ufs-diagnostics", &diagnostic, sizeof (diagnostic));
  gRootDiagnosticEnabled = diagnostic == 1;
  if (probe == 2) {
    inspect ();
  }
  if (probe == 8) {
    inspectContextBanks ();
  }
  if (probe == 7) {
    inspectSmmu ();
  }
  if (probe == 6) {
    dumpFirmwareList ();
  }
  if ((probe >= 3 && probe <= 5) || probe == 9) {
    adoptAndRead (probe);
  }
  if (probe == 10) {
    // Read-only check before anything is flashed: LUN 0 GPT -> "linux" -> its first block.
    const char *why = "";
    if (!hostBringUp (&why)) {
      panic ("NATIVE_UFS root check: bring-up failed: %s", why);
    }
    RootLayout *layout = static_cast<RootLayout *> (IOMalloc (sizeof (RootLayout)));
    if (!layout || !findRootLayout (layout, kRootPartition, &why)) {
      panic ("NATIVE_UFS root check: %s", why);
    }
    panic ("NATIVE_UFS root check\n"
           "partition first=%llx last=%llx\n"
           "block0 magic %08x %08x ext4magic=%04x ext4blocks=%u\n"
           "manifest status=%u windows=%u",
           layout->userdataFirst, layout->userdataLast, layout->magic0, layout->magic1,
           layout->ext4Magic, layout->ext4Blocks, layout->manifestStatus, layout->windows);
  }
  if (probe == 11) {
    // After the root image is flashed: manifest, then each window's own GPT (sector 1 of the
    // attached dmg disk image carries "EFI PART"). Read-only.
    const char *why = "";
    if (!hostBringUp (&why)) {
      panic ("NATIVE_UFS root windows: bring-up failed: %s", why);
    }
    RootLayout *layout = static_cast<RootLayout *> (IOMalloc (sizeof (RootLayout)));
    if (!layout || !findRootLayout (layout, kRootPartition, &why)) {
      panic ("NATIVE_UFS root windows: %s", why);
    }
    uint32_t sig[4][2] = {};
    uint32_t seen[4] = {};
    for (uint32_t index = 0; index < layout->windows && index < 4; index++) {
      uint64_t absolute = layout->rootFirst * 4096ULL + layout->window[index].offset;
      if (hostRead4k (0, absolute / 4096, 1, gScratch) == kIOReturnSuccess) {
        seen[index] = 1;
        sig[index][0] = le32 (gScratch + 512);
        sig[index][1] = le32 (gScratch + 516);
      }
    }
    panic ("NATIVE_UFS root windows\n"
           "partition first=%llx last=%llx manifest status=%u windows=%u\n"
           "w0 %s off=%llx size=%llx gpt=%08x %08x read=%u\n"
           "w1 %s off=%llx size=%llx gpt=%08x %08x read=%u\n"
           "w0 sha %08x %08x %08x %08x\n"
           "w1 sha %08x %08x %08x %08x",
           layout->userdataFirst, layout->userdataLast, layout->manifestStatus, layout->windows,
           layout->window[0].name, layout->window[0].offset, layout->window[0].size, sig[0][0], sig[0][1], seen[0],
           layout->window[1].name, layout->window[1].offset, layout->window[1].size, sig[1][0], sig[1][1], seen[1],
           be32w (layout->window[0].sha256), be32w (layout->window[0].sha256 + 4), be32w (layout->window[0].sha256 + 8), be32w (layout->window[0].sha256 + 12),
           be32w (layout->window[1].sha256), be32w (layout->window[1].sha256 + 4), be32w (layout->window[1].sha256 + 8), be32w (layout->window[1].sha256 + 12));
  }
  if (probe == 14) {
    // Immediate memory snapshot at root-device start, before the userspace that triggers jetsam.
    panic ("NATIVE_UFS early memory snapshot\n"
           "pages total=%u free=%u active=%u inactive=%u wired=%u spec=%u purgeable=%u\n"
           "available=%u max_mem=%llx procs=%d",
           vm_page_pages, vm_page_free_count, vm_page_active_count, vm_page_inactive_count,
           vm_page_wire_count, vm_page_speculative_count, vm_page_purgeable_count,
           memorystatus_available_pages, max_mem, nprocs);
  }
  if (probe == 13) {
    unsigned delay = 12;
    PE_parse_boot_argn ("native-ufs-stats-delay", &delay, sizeof (delay));
    if (!startRoot (provider)) {
      panic ("NATIVE_UFS memory snapshot: root start failed");
    }
    thread_t thread = nullptr;
    if (kernel_thread_start (statsThread, reinterpret_cast<void *> (static_cast<uintptr_t> (delay)), &thread) != KERN_SUCCESS) {
      panic ("NATIVE_UFS memory snapshot: cannot start the thread");
    }
    thread_deallocate (thread);
    return true;
  }
  if (probe == 12) {
    bool published = startRoot (provider);
    if (!published) {
      panic ("NATIVE_UFS media report: root start failed (blocks=%u)", native_ufs_blocks_published);
    }
    thread_t thread = nullptr;
    if (kernel_thread_start (mediaReportThread, nullptr, &thread) != KERN_SUCCESS) {
      panic ("NATIVE_UFS media report: cannot start the report thread");
    }
    thread_deallocate (thread);
    return true;
  }
  if (rootMode && !probe) {
    return startRoot (provider);
  }
  return false;
}

#ifdef NATIVE_UFS_BLOCK_DEVICE
// Boot the root image's windows as block devices. Soft-fails (logs, publishes nothing).
bool NativeUfs::startRoot (IOService *provider)
{
  const char *why = "";
  native_ufs_root_step = 1;
  if (!hostBringUp (&why)) {
    rootDiagnosticFailure (why);
    IOLog ("NativeUfs: root bring-up failed: %s\n", why);
    return false;
  }
  native_ufs_root_step = 2;
  RootLayout *layout = static_cast<RootLayout *> (IOMalloc (sizeof (RootLayout)));
  if (!layout || !findRootLayout (layout, kRootPartition, &why)) {
    rootDiagnosticFailure (layout ? why : "root layout allocation failed");
    IOLog ("NativeUfs: root layout failed: %s\n", why);
    if (layout) IOFree (layout, sizeof (RootLayout));
    return false;
  }
  native_ufs_root_step = 3;
  if (layout->manifestStatus != 0) {
    rootDiagnosticFailure ("root manifest rejected");
    IOLog ("NativeUfs: no valid root manifest in %s (status %u, magic %08x %08x)\n",
           kRootPartition, layout->manifestStatus, layout->magic0, layout->magic1);
    IOFree (layout, sizeof (RootLayout));
    return false;
  }
  gStoreGeometry = { true, layout->userdataFirst, layout->userdataLast, layout->imageBytes, layout->gptEntriesLba,
                     layout->gptHeaderCrc, layout->gptEntriesCrc, layout->gptEntryCount };
  const char *names[4] = {};
  for (unsigned i = 0; i < layout->windows; ++i) names[i] = layout->window[i].name;
  if (!ufsBuildRootPlan (names, layout->windows, gPublicationPlan)) {
    rootDiagnosticFailure ("root windows require distinct OS and SystemOS");
    IOFree (layout, sizeof (RootLayout));
    return false;
  }
  if (!IOService::start (provider)) {
    rootDiagnosticFailure ("root IOService start failed");
    IOFree (layout, sizeof (RootLayout));
    return false;
  }
  // Prepare all windows or publish none. Diagnostics never decide policy.
  gPending = 0;
  gPendingProvider = this;
  struct Operations {
    NativeUfs *owner;
    RootLayout *layout;
    bool prepare (unsigned index) {
      gRootDiagnostic.stage = "root window allocation/configuration";
      gRootDiagnostic.publicationPhase = static_cast<unsigned> (UfsRootPhase::Prepare);
      gRootDiagnostic.publicationWindow = index;
      gRootDiagnostic.windowBytes = layout->window[index].size;
      NativeUfsBlock *device = OSTypeAlloc (NativeUfsBlock);
      gPendingDevice[index] = device;
      gPendingAttached[index] = false;
      gPendingUuid[index][0] = 0;
      uint64_t absolute = layout->rootFirst * 4096ULL + layout->window[index].offset;
      if (!device || !device->init (static_cast<OSDictionary *> (nullptr)) ||
          !device->configure (layout->window[index].name, absolute, layout->window[index].size)) return false;
      gRootDiagnostic.stage = "root window attachment";
      if (!device->attach (owner)) return false;
      gPendingAttached[index] = true;
      ++gPending;
      gRootDiagnostic.prepared = gPending;
      return true;
    }
    bool validate () {
      gRootDiagnostic.stage = "signed window identities";
      gRootDiagnostic.publicationPhase = static_cast<unsigned> (UfsRootPhase::Identity);
#if defined(NATIVE_VERIFIED_BLOCK) && NATIVE_VERIFIED_BLOCK
      for (unsigned i = 0; i < gPending; ++i) {
        gRootDiagnostic.publicationWindow = i;
        if (!gPendingDevice[i]->signedVolumeUuid (gPendingUuid[i])) return false;
        for (unsigned j = 0; j < i; ++j) if (!strcmp (gPendingUuid[i], gPendingUuid[j])) return false;
      }
      return true;
#else
      // No owner trust is compiled in, so there is no signed policy to derive a volume UUID from, and
      // gPendingUuid stays empty. What this build can still check is what startRoot() itself arranged:
      // every window the plan named was prepared and attached. Readiness of the media each window
      // produces is decided by wait(), after start(); the root volume is then chosen by the kernel's
      // own /chosen/root-matching UUID, exactly as it is on the verified path.
      gRootDiagnostic.stage = "unsigned window set";
      for (unsigned i = 0; i < gPending; ++i) {
        if (!gPendingDevice[i] || !gPendingAttached[i]) return false;
        gPendingUuid[i][0] = 0;
      }
      return gPending == layout->windows;
#endif
    }
    bool schedule () {
      gRootDiagnostic.stage = "root publisher thread";
      gRootDiagnostic.publicationPhase = static_cast<unsigned> (UfsRootPhase::Schedule);
      gRootDiagnostic.publicationWindow = 0xffffffff;
      thread_t thread = nullptr;
      owner->retain ();
      if (kernel_thread_start (publishThread, nullptr, &thread) != KERN_SUCCESS) {
        owner->release ();
        return false;
      }
      thread_deallocate (thread);
      return true;
    }
    void discard (unsigned index) { discardPendingWindow (index); }
  } operations = { this, layout };
  UfsRootPublicationResult result = ufsPrepareRootWindows (operations, layout->windows);
  IOFree (layout, sizeof (RootLayout));
  if (result.phase != UfsRootPhase::Complete) {
    gPending = 0;
    gRootDiagnostic.result = result.phase == UfsRootPhase::Identity ? kIOReturnNotPrivileged :
      result.phase == UfsRootPhase::Schedule ? kIOReturnNoResources : kIOReturnError;
    IOLog ("NativeUfs: root preparation failed phase=%u window=%u; nothing published\n",
           static_cast<unsigned> (result.phase), result.index);
    IOService::stop (provider);
    rootDiagnosticFailure (result.phase == UfsRootPhase::Schedule ? "root publisher thread creation failed" :
                          result.phase == UfsRootPhase::Identity ? "signed window identities rejected" :
                          "root window preparation failed");
    return false;
  }
  return true;
}
#else // !NATIVE_UFS_BLOCK_DEVICE
// Probe-only build: the driver has no block device, so there is nothing to boot the root image as.
bool NativeUfs::startRoot (IOService *)
{
  panic ("NATIVE_UFS: this build has no block device (probe only)");
  return false;
}
#endif // NATIVE_UFS_BLOCK_DEVICE

void NativeUfs::stop (IOService *provider)
{
  fCmdDesc.reset ();
  fMap.reset ();
  IOService::stop (provider);
}
