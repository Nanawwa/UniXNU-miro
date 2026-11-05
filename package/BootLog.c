// SPDX-License-Identifier: MIT
// Diagnostic log on UFS (boot argument miro-log=1): every two seconds the kernel message buffer and the board text the diagnostic
// page would show are written to the last 32 MiB of the root partition through NativeUfs's bounded log window, so a run can be read
// back afterwards with scripts/read-miro-log.py. The layout follows the sheng writer (UniXNU-sheng/package/BootLog.c), without the
// sheng-only export, diagnostic-file and autoreset sections.
//
// Window layout (4096-byte blocks): 0 header, 1..32 the kernel message buffer (oldest first), 33.. text slots of 16 blocks in a ring.
// The header is written last in each round, so a reader never trusts a slot the header does not count yet.
#include <kern/thread.h>
#include <kern/clock.h>
#include <string.h>
#include <pexpert/pexpert.h>
#include <kern/misc_protos.h>
#include <IOKit/IOLib.h>

extern int native_ufs_log_open(uint64_t *, uint64_t *, const char **) __attribute__((weak_import));
extern int native_ufs_log_write(uint64_t, uint32_t, const void *) __attribute__((weak_import));
extern int native_ufs_log_sync(void) __attribute__((weak_import));
extern int native_ufs_log_probe(uint32_t, const char *) __attribute__((weak_import));
extern unsigned native_ufs_store_gpt_check(void) __attribute__((weak_import));
// The board diagnostic page is part of the optional boot-progress build. When it is absent these
// zero-filled fallbacks keep the writer linkable and simply log nothing for those rows.
__attribute__((weak)) char boot_progress_page_text[48][120];
__attribute__((weak)) char boot_progress_banner_text[49][120];
__attribute__((weak)) volatile unsigned boot_progress_banner_rows;
__attribute__((weak)) char boot_progress_panic_text[2048];
__attribute__((weak)) const char *boot_progress_tag(void) { return "miro"; }
extern unsigned long vm_kernel_slide;
extern char boot_process_rows[10][120];
extern void boot_process_snapshot(void);
extern void boot_process_capture(void);
extern volatile uint64_t native_data_persist_state __attribute__((weak_import));
extern volatile uint64_t native_data_persist_pages __attribute__((weak_import));
extern volatile uint64_t native_data_persist_loaded __attribute__((weak_import));
extern volatile uint64_t native_data_persist_writes __attribute__((weak_import));
extern volatile uint64_t native_data_persist_errors __attribute__((weak_import));
extern volatile uint64_t native_data_persist_first_lba __attribute__((weak_import));

// The kernel message buffer (bsd/sys/msgbuf.h); only these leading fields are read.
struct boot_log_msgbuf {int msg_magic, msg_size, msg_bufx, msg_bufr; char *msg_bufc;};
extern struct boot_log_msgbuf *msgbufp;

enum {MIRO_LOG_WINDOW_BLOCKS = 8192, LogBlock = 4096, LogMsgFirst = 1, LogMsgBlocks = 128, LogSlotFirst = 129, LogSlotBlocks = 16, LogPeriodMs = 2000, LogMaxErrors = 3};

struct boot_log_header {
  char magic[8];                 // "MIROLOG2"
  uint32_t version, header_bytes;
  uint64_t boot_id;              // CNTPCT when the writer started: tells one run from the next
  uint64_t first_lba;            // absolute 4096-byte LBA of block 0 on LUN 0
  uint32_t window_blocks, msg_first, msg_blocks, msg_bytes, slot_first, slot_blocks, slot_count, slots_written;
  uint32_t uptime_s, rounds, write_errors, gpt_open, gpt_last, state;
  char tag[32];
};

volatile uint32_t boot_log_state, boot_log_rounds, boot_log_errors;
volatile uint64_t boot_log_first_lba;
static int boot_log_started;
static uint8_t boot_log_buffer[LogMsgBlocks * LogBlock] __attribute__((aligned(LogBlock)));

static uint64_t log_ticks(void)
{
  uint64_t value;
  __asm__ volatile("mrs %0, cntpct_el0" : "=r"(value));
  return value;
}

// Copy the kernel message buffer into out (LogMsgBlocks * LogBlock bytes), oldest line first, and return the bytes used.
static uint32_t copy_msgbuf(void *out)
{
  struct boot_log_msgbuf *m = msgbufp;
  if (!m || !m->msg_bufc || m->msg_size <= 0 || !m->msg_bufx) return 0;
  char *dst = (char *)out;
  const uint32_t limit = LogMsgBlocks * LogBlock;
  uint32_t size = (uint32_t)m->msg_size, used = 0;
  if (size > limit) size = limit;
  uint32_t at = (uint32_t)m->msg_bufr % size;
  for (uint32_t i = 0; i < size; ++i) {
    dst[used] = m->msg_bufc[at];
    ++used;
    if (++at >= size) at = 0;
    if (used >= limit) break;
  }
  return used;
}

// Render the board text: diagnostic page rows, live banner, caught panic text and the Data store counters.
static void compose(char *out, unsigned bytes, unsigned round, unsigned now)
{
  memset(out, 0, bytes);
  unsigned at = 0;
  {
    for (unsigned row = 0; row < 48 && at + 121 <= bytes; ++row) {
      memcpy(out + at, boot_progress_page_text[row], 120);
      at += 121;
    }
  }
  unsigned banner = boot_progress_banner_rows;
  for (unsigned i = 0; i < banner && i < 49 && at + 121 <= bytes; ++i) {
    memcpy(out + at, boot_progress_banner_text[i], 120);
    at += 121;
  }
  if (&boot_process_capture) boot_process_capture();
  if (&boot_process_snapshot) boot_process_snapshot();
  for (unsigned i = 0; i < 10 && at + 121 <= bytes; ++i) {
    memcpy(out + at, boot_process_rows[i], 120);
    at += 121;
  }
  char line[160];
  unsigned n = 0;
  for (unsigned i = 0; i < sizeof boot_progress_panic_text && boot_progress_panic_text[i] && n < sizeof line - 1; ++i) {
    line[n++] = boot_progress_panic_text[i];
  }
  line[n] = 0;
  if (n && at + 161 <= bytes) {
    memcpy(out + at, line, 160);
    at += 161;
  }
  snprintf(line, sizeof line, "miro-log round %u t=%us slide=0x%lx", round, now, vm_kernel_slide);
  if (at + 161 <= bytes) { memcpy(out + at, line, 160); at += 161; }
  snprintf(line, sizeof line, "Data store state=%llu pages=%llu loaded=%llu writes=%llu errors=%llu first=%llu",
      &native_data_persist_state ? native_data_persist_state : 0,
      &native_data_persist_pages ? native_data_persist_pages : 0,
      &native_data_persist_loaded ? native_data_persist_loaded : 0,
      &native_data_persist_writes ? native_data_persist_writes : 0,
      &native_data_persist_errors ? native_data_persist_errors : 0,
      &native_data_persist_first_lba ? native_data_persist_first_lba : 0);
  if (at + 161 <= bytes) { memcpy(out + at, line, 160); at += 161; }
}

static void boot_log_writer_impl(int in_thread);
static void boot_log_writer(void *arg, wait_result_t) { boot_log_writer_impl(arg != 0); }
static void boot_log_writer_impl(int in_thread)
{
  uint64_t first = 0, blocks = 0;
  const char *why = 0;
  if (!&native_ufs_log_open || !&native_ufs_log_write || !&native_ufs_log_sync) {
    boot_log_state = 1;
    return;
  }
  if (!native_ufs_log_open(&first, &blocks, &why) || !blocks) {
    /* bsd_init already opened the window earlier; reuse it instead of refusing to write. */
    if (why && strcmp(why, "already open") == 0) {
      blocks = MIRO_LOG_WINDOW_BLOCKS;
      if (&native_ufs_log_probe) native_ufs_log_probe(0xE5, "reusing open window");
    } else {
      boot_log_state = 1;
      if (&native_ufs_log_probe) native_ufs_log_probe(0xE3, why ? why : "open refused");
      return;
    }
  } else if (&native_ufs_log_probe) {
    native_ufs_log_probe(0xE4, "window open");
  }
  boot_log_first_lba = first;
  const uint32_t slot_count = (uint32_t)((blocks - LogSlotFirst) / LogSlotBlocks);
  struct boot_log_header h;
  memset(&h, 0, sizeof h);
  memcpy(h.magic, "MIROLOG2", 8);
  h.version = 1; h.header_bytes = sizeof h;
  h.boot_id = log_ticks(); h.first_lba = first; h.window_blocks = (uint32_t)blocks;
  h.msg_first = LogMsgFirst; h.msg_blocks = LogMsgBlocks;
  h.slot_first = LogSlotFirst; h.slot_blocks = LogSlotBlocks; h.slot_count = slot_count;
  h.gpt_open = h.gpt_last = &native_ufs_store_gpt_check ? native_ufs_store_gpt_check() : 0xff;
  strlcpy(h.tag, boot_progress_tag(), sizeof h.tag);
  uint64_t f = 0;
  __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
  if (!f) f = 19200000;
  const uint64_t start = log_ticks();
  uint32_t errors = 0;
  boot_log_state = 2;
  int first_round = !in_thread;
  for (;;) {
    const unsigned now = (unsigned)((log_ticks() - start) / f);
    int ok = 1;
    memset(boot_log_buffer, 0, LogSlotBlocks * LogBlock);
    compose((char *)boot_log_buffer, LogSlotBlocks * LogBlock, h.slots_written, now);
    ok &= native_ufs_log_write(LogSlotFirst + (uint64_t)(h.slots_written % slot_count) * LogSlotBlocks, LogSlotBlocks, boot_log_buffer);
    memset(boot_log_buffer, 0, LogMsgBlocks * LogBlock);
    h.msg_bytes = copy_msgbuf(boot_log_buffer);
    ok &= native_ufs_log_write(LogMsgFirst, LogMsgBlocks, boot_log_buffer);
    if (ok) ++h.slots_written;
    else ++h.write_errors;
    h.uptime_s = now; h.rounds = ++boot_log_rounds; h.state = boot_log_state;
    if (&native_ufs_store_gpt_check && !(h.rounds % 30)) h.gpt_last = native_ufs_store_gpt_check();
    memset(boot_log_buffer, 0, LogBlock);
    memcpy(boot_log_buffer, &h, sizeof h);
    ok &= native_ufs_log_write(0, 1, boot_log_buffer);
    ok &= native_ufs_log_sync();
    if (!ok) {boot_log_errors = ++errors;} else errors = 0;
    if (errors >= LogMaxErrors) {boot_log_state = 3; return;}
    if (h.gpt_last != h.gpt_open) {boot_log_state = 4; return;}
    if (first_round) {
      /* The bootstrap caller keeps this round and hands the loop to one writer thread. */
      first_round = 0;
      thread_t thread;
      if (kernel_thread_start_priority(boot_log_writer, (void *)1, MINPRI_KERNEL, &thread) != KERN_SUCCESS) {
        boot_log_state = 5;
        return;
      }
      thread_deallocate(thread);
      return;
    }
    IOSleep(LogPeriodMs);
  }
}

// Start the writer the first time it is called with miro-log=1.
void boot_log_start(void)
{
  if (boot_log_started) return;
  boot_log_started = 1;
  uint32_t mode = 0;
  if (!PE_parse_boot_argn("miro-log", &mode, sizeof mode) || mode != 1) {
    if (&native_ufs_log_probe) native_ufs_log_probe(0xE0, "miro-log off");
    return;
  }
  if (!&native_ufs_log_open || !&native_ufs_log_write || !&native_ufs_log_sync) {
    if (&native_ufs_log_probe) native_ufs_log_probe(0xE1, "weak import missing");
    return;
  }
  if (&native_ufs_log_probe) native_ufs_log_probe(0xE2, "writer entering");
  boot_log_writer_impl(0);
}