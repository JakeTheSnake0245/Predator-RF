/* Predator RF — bridge between SDRPP module wrapper and vendored DSD-FME C code.
 *
 * The vendored DSD-FME expects to drive its own audio I/O (PulseAudio/OSS/sndfile/
 * ncurses) and tune its own RTL-SDR. We replace all of that with three thread-safe
 * ring buffers exposed through this header:
 *
 *   1) Sample INPUT ring  — SDRPP DSP graph -> FM-demodulated int16 @ 48 kHz mono
 *      pushed in by the wrapper. Pulled one sample at a time by the new
 *      `audio_in_type == 9` branch we patched into dsd_symbol.c::getSymbol().
 *
 *   2) Voice OUTPUT ring  — synthesized PCM @ 8 kHz int16 mono, captured from
 *      mbelib via our pa_simple_write() stub. Pulled by the wrapper, upsampled
 *      to 48 kHz, and fed into a SDRPP audio sink stream.
 *
 *   3) Metadata events    — keyed JSON-style payloads the wrapper converts into
 *      predator::DecoderIngestEvent rows for the Networks tab/Hits/Map.
 *
 * Threading: every function here is safe to call from any thread.
 */
#ifndef PREDATOR_DSD_BRIDGE_H
#define PREDATOR_DSD_BRIDGE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Marker value for opts->audio_in_type that routes getSymbol() into our path. */
#define PREDATOR_AUDIO_IN_TYPE 9

/* === Sample input ring (48 kHz int16 mono, FM-demodulated) === */
void   predator_dsd_push_input_samples(const int16_t *samples, size_t count);
int    predator_dsd_pull_input_sample(int16_t *out_sample);
size_t predator_dsd_input_pending(void);
void   predator_dsd_clear_input(void);

/* === Voice output ring (8 kHz int16 mono, post-mbelib synthesis) === */
void   predator_dsd_push_voice_samples(const int16_t *samples, size_t count);
size_t predator_dsd_pull_voice_samples(int16_t *out, size_t max_count);
size_t predator_dsd_voice_pending(void);
void   predator_dsd_clear_voice(void);

/* === Metadata events ===
 * Each event is a (protocol, kind, json_payload) triplet. The wrapper
 * installs a callback to receive them as they happen. */
typedef void (*predator_dsd_event_cb)(const char *protocol,
                                       const char *kind,
                                       const char *payload_json,
                                       void *userdata);

void   predator_dsd_set_event_cb(predator_dsd_event_cb cb, void *userdata);
void   predator_dsd_emit_event(const char *protocol,
                                const char *kind,
                                const char *payload_json);

/* === Lifecycle ===
 * The vendored code's loops check `exitflag` to decide whether to keep going.
 * The wrapper sets running=1 on start, running=0 on stop. The stubs honour it. */
void   predator_dsd_set_running(int running);
int    predator_dsd_is_running(void);

/* === Decoder worker entry points (Phase 3b) ===
 *
 * predator_dsd_init_decoder():
 *   One-shot: zero+default the global dsd_opts/dsd_state via the vendored
 *   initOpts()+initState(), wire init_audio_filters(), init_rrc_filter_memory(),
 *   InitAllFecFunction(), then mark audio_in_type=PREDATOR_AUDIO_IN_TYPE so
 *   getSymbol() reads from our input ring instead of pulse/oss/rtl. Safe to
 *   call multiple times; subsequent calls are no-ops.
 *
 * predator_dsd_run_decoder_loop():
 *   BLOCKING. Runs the upstream liveScanner() body until exitflag flips.
 *   The SDRPP wrapper spawns a std::thread for this and signals stop via
 *   predator_dsd_set_running(0) on shutdown.
 */
void   predator_dsd_init_decoder(void);
void   predator_dsd_run_decoder_loop(void);

#ifdef __cplusplus
}
#endif

#endif /* PREDATOR_DSD_BRIDGE_H */
