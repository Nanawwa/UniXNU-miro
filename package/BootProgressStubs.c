// SPDX-License-Identifier: MIT
// Weak fallbacks for the sheng-only diagnostics that BootProgress.c reads. The miro port does not build the
// Virtio block transport, the ADSP firmware loader, the cpufreq probe, the touch driver or the legacy display
// probes, so every counter reads zero. A board that later selects those drivers provides the strong definitions.
#include <stdint.h>

__attribute__((weak)) uint64_t boot_process_attempts;
__attribute__((weak)) uint64_t boot_process_busy;
__attribute__((weak)) uint64_t boot_process_passes;
__attribute__((weak)) uint64_t boot_process_rows;
__attribute__((weak)) uint64_t boot_process_seen;
__attribute__((weak)) uint64_t boot_ui_trace_index;
__attribute__((weak)) uint64_t boot_ui_trace_label;
__attribute__((weak)) uint64_t boot_ui_trace_phase;
__attribute__((weak)) uint64_t boot_ui_trace_ready;
__attribute__((weak)) uint64_t boot_ui_trace_role_rows;
__attribute__((weak)) uint64_t boot_ui_trace_rows;
__attribute__((weak)) uint64_t native_adreno_audit_attr;
__attribute__((weak)) uint64_t native_adreno_audit_ticks;
__attribute__((weak)) uint64_t native_adreno_cache_stats;
__attribute__((weak)) uint64_t native_adreno_coherence;
__attribute__((weak)) uint64_t native_adreno_reassert_ticks;
__attribute__((weak)) uint64_t native_adreno_wc_audit;
__attribute__((weak)) uint64_t native_adreno_wc_distrusted;
__attribute__((weak)) uint64_t native_cpufreq_row;
__attribute__((weak)) uint64_t native_cpufreq_stage;
__attribute__((weak)) uint64_t native_gop_gpu_abandoned;
__attribute__((weak)) uint64_t native_gop_gpu_completions;
__attribute__((weak)) uint64_t native_gop_gpu_failures;
__attribute__((weak)) uint64_t native_gop_gpu_offers;
__attribute__((weak)) uint64_t native_gop_virtual;
__attribute__((weak)) uint64_t native_hid_input_events;
__attribute__((weak)) uint64_t native_hid_input_results;
__attribute__((weak)) uint64_t native_legacy_client_calls;
__attribute__((weak)) uint64_t native_legacy_client_class;
__attribute__((weak)) uint64_t native_legacy_client_pid;
__attribute__((weak)) uint64_t native_legacy_client_status;
__attribute__((weak)) uint64_t native_legacy_client_type;
__attribute__((weak)) uint64_t native_legacy_copy_bytes_last;
__attribute__((weak)) uint64_t native_legacy_copy_bytes_sum;
__attribute__((weak)) uint64_t native_legacy_copy_last;
__attribute__((weak)) uint64_t native_legacy_copy_max;
__attribute__((weak)) uint64_t native_legacy_copy_n;
__attribute__((weak)) uint64_t native_legacy_copy_sum;
__attribute__((weak)) uint64_t native_legacy_early_failures;
__attribute__((weak)) uint64_t native_legacy_early_retires;
__attribute__((weak)) uint64_t native_legacy_frame_color_counts;
__attribute__((weak)) uint64_t native_legacy_frame_color_other;
__attribute__((weak)) uint64_t native_legacy_frame_colors;
__attribute__((weak)) uint64_t native_legacy_frame_count;
__attribute__((weak)) uint64_t native_legacy_frame_kind;
__attribute__((weak)) uint64_t native_legacy_frame_stats;
__attribute__((weak)) uint64_t native_legacy_frame_time;
__attribute__((weak)) uint64_t native_legacy_gop_bytes;
__attribute__((weak)) uint64_t native_legacy_gop_physical;
__attribute__((weak)) uint64_t native_legacy_gop_virtual;
__attribute__((weak)) uint64_t native_legacy_gpu_fallbacks;
__attribute__((weak)) uint64_t native_legacy_gpu_last_status;
__attribute__((weak)) uint64_t native_legacy_guard_slot;
__attribute__((weak)) uint64_t native_legacy_pm_callbacks;
__attribute__((weak)) uint64_t native_legacy_pm_calls;
__attribute__((weak)) uint64_t native_legacy_pm_stage;
__attribute__((weak)) uint64_t native_legacy_prepare_attempts;
__attribute__((weak)) uint64_t native_legacy_prepare_calls;
__attribute__((weak)) uint64_t native_legacy_prepare_reason;
__attribute__((weak)) uint64_t native_legacy_rej_class;
__attribute__((weak)) uint64_t native_legacy_rej_reason;
__attribute__((weak)) uint64_t native_legacy_rej_size;
__attribute__((weak)) uint64_t native_legacy_rej_state;
__attribute__((weak)) uint64_t native_legacy_rej_surface;
__attribute__((weak)) uint64_t native_legacy_rej_surfaces;
__attribute__((weak)) uint64_t native_legacy_retirement_calls;
__attribute__((weak)) uint64_t native_legacy_stage;
__attribute__((weak)) uint64_t native_legacy_swap_accepted_count;
__attribute__((weak)) uint64_t native_legacy_swap_rejected_count;
__attribute__((weak)) uint64_t native_legacy_swap_validation_calls;
__attribute__((weak)) uint64_t native_legacy_swap_validation_reason;
__attribute__((weak)) uint64_t native_legacy_swap_validation_result;
__attribute__((weak)) uint64_t native_legacy_transfer_bytes;
__attribute__((weak)) uint64_t native_legacy_transfer_calls;
__attribute__((weak)) uint64_t native_legacy_transfer_status;
__attribute__((weak)) uint64_t native_owner_root_accepted;
__attribute__((weak)) uint64_t native_touch_count;
__attribute__((weak)) uint64_t native_touch_rows;
__attribute__((weak)) uint64_t native_touch_seq;
__attribute__((weak)) uint64_t native_touch_stage_req;
__attribute__((weak)) uint64_t native_touch_xs;
__attribute__((weak)) uint64_t native_touch_ys;
__attribute__((weak)) uint64_t native_ufs_integrity_failures;
__attribute__((weak)) uint64_t native_ufs_verified_blocks;

__attribute__((weak)) int qcom_mdt_split(const void *a, uint32_t b, const char *c, void *d, uint32_t *e) { (void)a; (void)b; (void)c; (void)d; (void)e; return 0; }
__attribute__((weak)) int qcom_mdt_split_read(const char *a, uint32_t b, void *c, uint32_t d) { (void)a; (void)b; (void)c; (void)d; return 0; }
__attribute__((weak)) int native_ufs_part_read(uint32_t a, uint64_t b, uint32_t c, void *d) { (void)a; (void)b; (void)c; (void)d; return 0; }
__attribute__((weak)) int native_ufs_part_find(const char *a, uint64_t *b, uint64_t *c) { (void)a; (void)b; (void)c; return 0; }
__attribute__((weak)) int fat_mount(void *a) { (void)a; return 0; }
__attribute__((weak)) int fat_open(const char *a, void **b) { (void)a; (void)b; return 0; }
__attribute__((weak)) int fat_read(void *a, void *b, uint32_t c) { (void)a; (void)b; (void)c; return 0; }
__attribute__((weak)) int boot_process_capture(unsigned a) { (void)a; return 0; }
__attribute__((weak)) int boot_process_snapshot(void) { return 0; }
__attribute__((weak)) int boot_ui_trace_snapshot(unsigned a) { (void)a; return 0; }
__attribute__((weak)) int sheng_cpufreq_poll(void) { return 0; }
__attribute__((weak)) int boot_log_tick(unsigned a) { (void)a; return 0; }
__attribute__((weak)) uint64_t native_ufs_verify_ticks;
__attribute__((weak)) uint64_t native_virtio_read_failures;
__attribute__((weak)) uint64_t native_virtio_read_requests;
__attribute__((weak)) uint64_t native_virtio_ready;
__attribute__((weak)) uint64_t native_virtio_verified_blocks;
