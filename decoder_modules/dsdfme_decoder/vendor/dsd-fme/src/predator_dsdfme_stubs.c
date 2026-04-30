/* Predator RF — DSD-FME stub layer.
 *
 * Provides:
 *   1) No-op implementations of every external symbol the kept .c files
 *      reference but no longer have a real backing library:
 *        - PulseAudio (pa_simple_*, pa_strerror)
 *        - libsndfile (sf_open*, sf_close, sf_read_short, sf_write_short, sf_strerror)
 *        - rtl-sdr   (get_rtlsdr_sample, rtl_dev_tune, rtl_return_rms, init_rtl_stream)
 *        - dsd-fme   (cleanupAndExit, Connect, openPulseInput) defined in
 *                    excluded files (dsd_main.c / dsd_rigctl.c / dsd_ncurses*.c).
 *
 *   2) Three lock-protected ring buffers exposed via predator_dsd_bridge.h
 *      that connect the vendored code to the SDRPP module wrapper:
 *        - input ring   (SDRPP -> getSymbol)
 *        - voice ring   (mbelib -> SDRPP audio sink)
 *        - event channel (DSD-FME state changes -> Networks tab)
 *
 *   3) The interception point: our pa_simple_write() stub pushes samples into
 *      the voice ring instead of writing to PulseAudio. That single hook is
 *      how we capture P25/DMR voice without modifying any DSD-FME source.
 */

#include "../../src/predator_dsd_bridge.h"
#include "dsd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>

/* Vendored DSD-FME defines exitflag in dsd_main.c. We compile our own copy
 * here because dsd_main.c is excluded from the Predator build. */
volatile uint8_t exitflag = 0;

/* ============================================================
 * Ring-buffer primitive (single-producer / single-consumer style,
 * but mutex-guarded so multi-threaded callers are safe).
 * ============================================================ */

#define INPUT_RING_CAP  (48000 * 4)   /* 4 s of 48 kHz samples */
#define VOICE_RING_CAP  ( 8000 * 8)   /* 8 s of 8 kHz samples  */

typedef struct {
    int16_t        *buf;
    size_t          cap;
    size_t          head;       /* write index */
    size_t          tail;       /* read index  */
    size_t          count;
    pthread_mutex_t lock;
} predator_ring_t;

static int ring_init(predator_ring_t *r, size_t cap) {
    r->buf = (int16_t*)calloc(cap, sizeof(int16_t));
    if (!r->buf) return -1;
    r->cap   = cap;
    r->head  = 0;
    r->tail  = 0;
    r->count = 0;
    pthread_mutex_init(&r->lock, NULL);
    return 0;
}

static void ring_push(predator_ring_t *r, const int16_t *src, size_t n) {
    pthread_mutex_lock(&r->lock);
    for (size_t i = 0; i < n; i++) {
        if (r->count == r->cap) {
            /* drop oldest */
            r->tail = (r->tail + 1) % r->cap;
            r->count--;
        }
        r->buf[r->head] = src[i];
        r->head = (r->head + 1) % r->cap;
        r->count++;
    }
    pthread_mutex_unlock(&r->lock);
}

static size_t ring_pull(predator_ring_t *r, int16_t *dst, size_t max) {
    size_t n = 0;
    pthread_mutex_lock(&r->lock);
    while (n < max && r->count > 0) {
        dst[n++] = r->buf[r->tail];
        r->tail = (r->tail + 1) % r->cap;
        r->count--;
    }
    pthread_mutex_unlock(&r->lock);
    return n;
}

static size_t ring_count(predator_ring_t *r) {
    pthread_mutex_lock(&r->lock);
    size_t c = r->count;
    pthread_mutex_unlock(&r->lock);
    return c;
}

static void ring_clear(predator_ring_t *r) {
    pthread_mutex_lock(&r->lock);
    r->head = r->tail = r->count = 0;
    pthread_mutex_unlock(&r->lock);
}

/* ============================================================
 * Three rings + event channel state.
 * ============================================================ */

static predator_ring_t g_input;
static predator_ring_t g_voice;
static int g_rings_initialized = 0;
static pthread_mutex_t g_init_lock = PTHREAD_MUTEX_INITIALIZER;

static volatile int g_running = 0;

static predator_dsd_event_cb g_event_cb = NULL;
static void                 *g_event_userdata = NULL;
static pthread_mutex_t       g_event_lock = PTHREAD_MUTEX_INITIALIZER;

static void ensure_init(void) {
    pthread_mutex_lock(&g_init_lock);
    if (!g_rings_initialized) {
        ring_init(&g_input, INPUT_RING_CAP);
        ring_init(&g_voice, VOICE_RING_CAP);
        g_rings_initialized = 1;
    }
    pthread_mutex_unlock(&g_init_lock);
}

/* ============================================================
 * Public bridge API (predator_dsd_bridge.h).
 * ============================================================ */

void predator_dsd_push_input_samples(const int16_t *samples, size_t count) {
    ensure_init();
    ring_push(&g_input, samples, count);
}

int predator_dsd_pull_input_sample(int16_t *out_sample) {
    ensure_init();
    if (!g_running) return -1;
    int16_t s;
    if (ring_pull(&g_input, &s, 1) == 0) {
        /* No data — return 0 silence rather than blocking the symbol loop. */
        *out_sample = 0;
        return 0;
    }
    *out_sample = s;
    return 0;
}

size_t predator_dsd_input_pending(void) { ensure_init(); return ring_count(&g_input); }
void   predator_dsd_clear_input(void)   { ensure_init(); ring_clear(&g_input); }

void predator_dsd_push_voice_samples(const int16_t *samples, size_t count) {
    ensure_init();
    ring_push(&g_voice, samples, count);
}

size_t predator_dsd_pull_voice_samples(int16_t *out, size_t max_count) {
    ensure_init();
    return ring_pull(&g_voice, out, max_count);
}

size_t predator_dsd_voice_pending(void) { ensure_init(); return ring_count(&g_voice); }
void   predator_dsd_clear_voice(void)   { ensure_init(); ring_clear(&g_voice); }

void predator_dsd_set_event_cb(predator_dsd_event_cb cb, void *userdata) {
    pthread_mutex_lock(&g_event_lock);
    g_event_cb = cb;
    g_event_userdata = userdata;
    pthread_mutex_unlock(&g_event_lock);
}

void predator_dsd_emit_event(const char *protocol, const char *kind, const char *payload_json) {
    pthread_mutex_lock(&g_event_lock);
    predator_dsd_event_cb cb = g_event_cb;
    void *ud = g_event_userdata;
    pthread_mutex_unlock(&g_event_lock);
    if (cb) cb(protocol ? protocol : "DSDFME",
                kind     ? kind     : "info",
                payload_json ? payload_json : "{}",
                ud);
}

void predator_dsd_set_running(int running) {
    g_running = running;
    exitflag  = running ? 0 : 1;
}

int predator_dsd_is_running(void) { return g_running; }

/* ============================================================
 * Stubs for excluded dsd-fme TUs (dsd_main.c / dsd_rigctl.c /
 * dsd_ncurses_*.c / pa_devs.c / pulse_devices.c / dsd_serial.c /
 * dsd_import.c).  Any cross-TU references from the kept files
 * resolve here.
 * ============================================================ */

void cleanupAndExit(dsd_opts *opts, dsd_state *state) {
    (void)opts; (void)state;
    /* Don't exit() the process — just unwind the worker thread loop. */
    g_running = 0;
    exitflag  = 1;
}

int Connect(char *hostname, int portno) {
    (void)hostname; (void)portno;
    return -1;  /* no rigctl in Predator build */
}

/* NOTE: openPulseInput / openPulseOutput / openOSSOutput live in dsd_audio.c
 * (kept). Their pulse-/OSS-/sndfile branches now resolve via the stubbed
 * pa_simple_* / sf_* / OSS-ioctl macros above. We intentionally do *not*
 * redefine them here — that would cause multiple-definition link errors. */

/* RTL-SDR direct-tune helpers: replaced with metadata events so the operator
 * still sees what the trunking decoder *wants* to retune to. */
void rtl_dev_tune(dsd_opts *opts, long int frequency) {
    (void)opts;
    char buf[96];
    snprintf(buf, sizeof(buf), "{\"requested_freq_hz\":%ld}", frequency);
    predator_dsd_emit_event("DSDFME", "retune_request", buf);
}

int get_rtlsdr_sample(short *sample, dsd_opts *opts, dsd_state *state) {
    (void)opts; (void)state;
    /* When audio_in_type == 9 (Predator) we never enter this path; this stub
     * exists only to satisfy the link of branches that reference it. */
    if (sample) *sample = 0;
    return 0;
}

short rtl_return_rms(void) { return 0; }

void  init_rtl_stream(dsd_opts *opts) { (void)opts; }
void  rtl_clean_queue(dsd_opts *opts) { (void)opts; }

/* ============================================================
 * PulseAudio stubs.  pa_simple_write is the magic interception
 * point — it's what processSynthesizedVoice paths call to ship
 * mbelib-decoded PCM to the audio device. We grab it and push
 * to our voice ring instead.
 * ============================================================ */

struct pa_simple { int dummy; };

pa_simple *pa_simple_new(void *server, const char *name, int dir,
                          const char *dev, const char *stream_name,
                          const void *ss, const void *map, const void *attr,
                          int *error) {
    (void)server; (void)name; (void)dir; (void)dev; (void)stream_name;
    (void)ss; (void)map; (void)attr;
    if (error) *error = 0;
    /* Return a non-null sentinel so the caller's NULL-check passes. */
    static pa_simple sentinel = { 0 };
    return &sentinel;
}

int pa_simple_write(pa_simple *s, const void *data, size_t bytes, int *error) {
    (void)s;
    if (error) *error = 0;
    /* Predator: capture mbelib-synthesized voice. Bytes are int16 PCM samples. */
    if (data && bytes >= 2) {
        predator_dsd_push_voice_samples((const int16_t*)data, bytes / sizeof(int16_t));
    }
    return 0;
}

int pa_simple_read(pa_simple *s, void *data, size_t bytes, int *error) {
    (void)s;
    if (error) *error = 0;
    /* Predator's input path is audio_in_type == 9; this should never run.
     * Return zeroed samples just in case. */
    if (data && bytes) memset(data, 0, bytes);
    return 0;
}

void pa_simple_free(pa_simple *s) { (void)s; }

unsigned long pa_simple_get_latency(pa_simple *s, int *error) {
    (void)s; if (error) *error = 0; return 0;
}

const char *pa_strerror(int e) { (void)e; return "ok"; }

/* ============================================================
 * libsndfile stubs.  We never read or write WAV files in the
 * Predator build, but several .c files reference these symbols.
 * ============================================================ */

struct sndfile_stub { int dummy; };
struct sf_info_stub { int dummy; };

SNDFILE *sf_open(const char *path, int mode, SF_INFO *info) {
    (void)path; (void)mode; (void)info; return NULL;
}
SNDFILE *sf_open_fd(int fd, int mode, SF_INFO *info, int close_desc) {
    (void)fd; (void)mode; (void)info; (void)close_desc; return NULL;
}
int      sf_close(SNDFILE *s) { (void)s; return 0; }
long     sf_read_short (SNDFILE *s, short *p, long n) { (void)s; (void)p; (void)n; return 0; }
long     sf_write_short(SNDFILE *s, const short *p, long n) { (void)s; (void)p; (void)n; return n; }
void     sf_write_sync (SNDFILE *s) { (void)s; }
const char *sf_strerror(SNDFILE *s) { (void)s; return "no sndfile in Predator build"; }

/* ============================================================
 * SF_INFO macros used as bare identifiers (SF_FORMAT_RAW etc.)
 * Provided as int constants so dsd_symbol.c's TCP retry path
 * still compiles even though it never runs in the Predator build.
 * ============================================================ */
#ifndef SF_FORMAT_RAW
#  define SF_FORMAT_RAW       0x000020
#endif
#ifndef SF_FORMAT_PCM_16
#  define SF_FORMAT_PCM_16    0x000002
#endif
#ifndef SF_ENDIAN_LITTLE
#  define SF_ENDIAN_LITTLE    0x10000000
#endif
#ifndef SFM_READ
#  define SFM_READ            0x10
#endif

/* ============================================================
 * Stubs for symbols defined in EXCLUDED dsd_rigctl.c.
 * Trunking handlers (dmr_csbk.c, p25 trunking, noCarrier) reference these
 * but only inside `if (opts->use_rigctl == 1) { ... }` branches that we
 * never enter (initOpts sets use_rigctl=0 and there's no UI to flip it).
 * They exist purely to satisfy the linker.
 * ============================================================ */
void SetModulation(int sockfd, int bw)        { (void)sockfd; (void)bw;        }
void SetFreq      (int sockfd, long freq_hz)  { (void)sockfd; (void)freq_hz;   }

/* ============================================================
 * Stub for the symbol defined in EXCLUDED dsd_ncurses_printer.c.
 * liveScanner()'s `if (use_ncurses_terminal == 1) ncursesOpen(...)` is
 * additionally gated by `#ifndef PREDATOR_BUILD` in dsd_main.c, but a
 * couple of other TUs (dsd_audio.c diagnostic paths) still reference it.
 * ============================================================ */
void ncursesOpen (dsd_opts *opts, dsd_state *state) { (void)opts; (void)state; }

/* ============================================================
 * Predator decoder worker (Phase 3b runtime hookup).
 *
 * Wires the SDRPP module wrapper to the vendored DSD-FME decoder loop:
 *
 *   predator_dsd_init_decoder()
 *     -> initOpts()  / initState()  / init_audio_filters()
 *     -> init_rrc_filter_memory()   / InitAllFecFunction()
 *     -> override audio_in_type = PREDATOR_AUDIO_IN_TYPE so getSymbol()
 *        reads from our 48 kHz int16 input ring (see dsd_symbol.c patch).
 *     -> override audio_out_type = 0 so the writeSynthesizedVoice() path
 *        in dsd_audio.c falls into the pulse branch — and our pa_simple_*
 *        stubs above push synthesized PCM into the voice ring.
 *
 *   predator_dsd_run_decoder_loop()
 *     -> blocking call to the vendored liveScanner() (still defined in
 *        the un-gated portion of dsd_main.c). Returns when exitflag flips,
 *        which the wrapper does on disable() via predator_dsd_set_running(0).
 * ============================================================ */

extern void initOpts             (dsd_opts  *opts);
extern void initState            (dsd_state *state);
extern void init_audio_filters   (dsd_state *state);
extern void init_rrc_filter_memory (void);
extern void InitAllFecFunction   (void);
extern void liveScanner          (dsd_opts *opts, dsd_state *state);

static dsd_opts        g_dsd_opts;
static dsd_state       g_dsd_state;
static int             g_decoder_initialized = 0;
static pthread_mutex_t g_init_decoder_lock   = PTHREAD_MUTEX_INITIALIZER;

void predator_dsd_init_decoder(void) {
    pthread_mutex_lock(&g_init_decoder_lock);
    if (!g_decoder_initialized) {
        /* Heavy zero+default. initState() mallocs ~6 MB of working buffers
         * (dibit_buf, dmr_payload_buf, audio_out_buf*, mbe parm structs,
         * event history) so we run it exactly once and reuse. */
        initOpts (&g_dsd_opts);
        initState(&g_dsd_state);
        init_audio_filters(&g_dsd_state);
        init_rrc_filter_memory();
        InitAllFecFunction();

        /* Predator overrides on top of the desktop defaults. */
        g_dsd_opts.audio_in_type        = PREDATOR_AUDIO_IN_TYPE;  /* 9 -> our ring */
        g_dsd_opts.audio_out_type       = 0;                       /* pulse path -> our pa_simple_write stub */
        g_dsd_opts.use_ncurses_terminal = 0;
        g_dsd_opts.use_rigctl           = 0;

        /* Bring up the protocols Predator cares about. The desktop defaults
         * already enable these but make it explicit so behaviour can't drift
         * with future upstream merges. P25 P1+P2 + DMR + NXDN + YSF stay on,
         * D-STAR and X2-TDMA off (rare in our threat model). */
        g_dsd_opts.frame_p25p1   = 1;
        g_dsd_opts.frame_p25p2   = 1;
        g_dsd_opts.frame_dmr     = 1;
        g_dsd_opts.frame_nxdn48  = 1;
        g_dsd_opts.frame_nxdn96  = 1;
        g_dsd_opts.frame_ysf     = 1;
        g_dsd_opts.frame_dstar   = 0;
        g_dsd_opts.frame_x2tdma  = 0;
        g_dsd_opts.frame_dpmr    = 0;
        g_dsd_opts.frame_provoice = 0;
        g_dsd_opts.frame_m17     = 0;

        /* Sentinel non-NULL pulse handle so writeSynthesizedVoice's
         * `if (opts->pulse_digi_dev_out != NULL)` branch fires. The stub
         * pa_simple_new() returns its own internal sentinel for any
         * subsequent re-open call — we just need a non-NULL placeholder
         * here in case downstream code checks before any pa_simple_new(). */
        static struct pa_simple sentinel = { 0 };
        g_dsd_opts.pulse_digi_dev_out = &sentinel;
        g_dsd_opts.pulse_raw_dev_out  = &sentinel;

        g_decoder_initialized = 1;
    }
    pthread_mutex_unlock(&g_init_decoder_lock);
}

void predator_dsd_run_decoder_loop(void) {
    predator_dsd_init_decoder();
    /* Reset exit flag in case a prior session left it set. */
    exitflag = 0;
    /* Blocks until exitflag != 0 (set by predator_dsd_set_running(0) or
     * the stubbed cleanupAndExit above). */
    liveScanner(&g_dsd_opts, &g_dsd_state);
}
