/*
    Predator RF — no-op stubs for rtl_433 SDR / network-output / mongoose /
    sigrok / terminal symbols that the Android build does not ship.

    Why this file exists:
      * rtl_433 ships a desktop SDR layer (sdr.c), an embedded networking
        layer (mongoose.c), an rtl_tcp raw-output server (output_rtltcp.c),
        a Pulseview/sigrok dumper (write_sigrok.c), and a TTY color/control
        layer (term_ctl.c) for use on desktop Linux/Windows.
      * Predator RF on Android does not need any of that:
          - Samples come from the SDRPP DSP graph (we feed them in directly).
          - Networking/location/persistence go through the Android stack and
            our own bridge ingesters.
          - Output is consumed in-process by our PredatorDataOutput, not via
            stdout colour escapes or sigrok captures.
      * We therefore exclude sdr.c, mongoose.c, http_server.c, write_sigrok.c,
        output_rtltcp.c, term_ctl.c, and the
        output_{mqtt,influx,trigger,udp}.c sources from the build.
      * But r_api.c, data_tag.c, and output_file.c still call into those
        symbols (gated by runtime config Predator never sets, or as a
        zero-effect colour layer). To keep the linker happy we provide
        harmless no-op definitions here.

    Every function in this file MUST be a no-op. None of them are reachable
    along Predator's enabled code paths — they exist purely for link-time
    symbol resolution.
*/

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>

#include "sdr.h"
#include "mongoose.h"
#include "data.h"
#include "term_ctl.h"

/* Forward decls for the rtl_tcp + sigrok writers — their headers are not
   included by r_api.c (it relies on implicit declarations there). We put
   matching prototypes here so the function definitions below have the same
   linkage and types they would have had if their owning .c files were in
   the build. */
struct raw_output;
struct r_cfg;
struct raw_output *raw_output_rtltcp_create(char const *host, char const *port,
                                            char const *opts, struct r_cfg *cfg);
void write_sigrok(char const *filename, unsigned samplerate, unsigned probes,
                  unsigned analogs, char const *labels[]);
void open_pulseview(char const *filename);

/* ---------- SDR layer ----------
   Predator drives the rtl_433 pulse pipeline directly with samples from
   the SDRPP DSP graph, so all of rtl_433's tuner / sample-rate / device
   control surface is a no-op here. */

int sdr_close(sdr_dev_t *dev)                                    { (void)dev; return 0; }
int sdr_deactivate(sdr_dev_t *dev)                                { (void)dev; return 0; }
int sdr_set_center_freq(sdr_dev_t *dev, uint32_t f, int v)        { (void)dev; (void)f; (void)v; return 0; }
int sdr_set_freq_correction(sdr_dev_t *dev, int ppm, int v)       { (void)dev; (void)ppm; (void)v; return 0; }
int sdr_set_sample_rate(sdr_dev_t *dev, uint32_t r, int v)        { (void)dev; (void)r; (void)v; return 0; }
int sdr_set_tuner_gain(sdr_dev_t *dev, char const *g, int v)      { (void)dev; (void)g; (void)v; return 0; }

/* ---------- Mongoose ----------
   The gpsd_client block in data_tag.c references these symbols but is
   only invoked when the user passes a "gpsd://..." tag — Predator never
   does. */

void mg_mgr_init(struct mg_mgr *mgr, void *user_data) {
    if (mgr) mgr->opaque = NULL;
    (void)user_data;
}

void mg_mgr_free(struct mg_mgr *mgr)                                  { (void)mgr; }
void mg_send(struct mg_connection *nc, const void *buf, int len)      { (void)nc; (void)buf; (void)len; }

struct mg_connection *mg_connect_opt(struct mg_mgr *mgr,
                                     const char *address,
                                     mg_event_handler_t handler,
                                     struct mg_connect_opts opts) {
    (void)mgr; (void)address; (void)handler; (void)opts;
    return NULL;
}

/* mbuf_remove is part of the mongoose mbuf API; the gpsd block in
   data_tag.c calls it to discard a parsed line from the recv buffer. With
   the gpsd path disabled this is unreachable, so a no-op is fine. */
void mbuf_remove(struct mbuf *mb, size_t len) { (void)mb; (void)len; }

/* ---------- Network output factories ----------
   r_api.c links these via add_*_output() helpers that Predator never
   calls. Returning NULL keeps r_api.c's list_push happy (NULL elements
   are tolerated by the output dispatch loop, see r_api.c's data_acquired
   handler which iterates output_handler skipping NULLs). */

struct data_output *data_output_mqtt_create(struct mg_mgr *mgr, char *param, char const *dev_hint) {
    (void)mgr; (void)param; (void)dev_hint; return NULL;
}

struct data_output *data_output_influx_create(struct mg_mgr *mgr, char *opts) {
    (void)mgr; (void)opts; return NULL;
}

struct data_output *data_output_syslog_create(int log_level, const char *host, const char *port) {
    (void)log_level; (void)host; (void)port; return NULL;
}

struct data_output *data_output_http_create(struct mg_mgr *mgr,
                                            const char *host,
                                            const char *port,
                                            struct r_cfg *cfg) {
    (void)mgr; (void)host; (void)port; (void)cfg; return NULL;
}

struct data_output *data_output_trigger_create(FILE *file) {
    (void)file; return NULL;
}

/* ---------- rtl_tcp raw output ----------
   Reachable only via add_rtltcp_output() (CLI -F rtl_tcp). Predator's
   module never wires that path. */
struct raw_output *raw_output_rtltcp_create(char const *host, char const *port,
                                            char const *opts, struct r_cfg *cfg) {
    (void)host; (void)port; (void)opts; (void)cfg;
    return NULL;
}

/* ---------- Sigrok / Pulseview ----------
   Reachable only via the -W file-output mode (cfg->sr_filename set). The
   module never sets sr_filename, so this is unreachable. */
void write_sigrok(char const *filename, unsigned samplerate, unsigned probes,
                  unsigned analogs, char const *labels[]) {
    (void)filename; (void)samplerate; (void)probes; (void)analogs; (void)labels;
}

void open_pulseview(char const *filename) { (void)filename; }

/* ---------- Terminal control ----------
   output_file.c calls term_init(stdout) once, then guards every call on
   ctx != NULL. Returning NULL here makes every term_* call a no-op
   inside output_file.c, so the formatter just runs without colour
   escapes — exactly what we want on Android (no TTY anyway). The few
   APIs whose callers don't NULL-check (term_get_columns,
   term_has_color) get explicit zero returns. */

void *term_init(FILE *fp)                                  { (void)fp; return NULL; }
void  term_free(void *ctx)                                 { (void)ctx; }
int   term_get_columns(void *ctx)                          { (void)ctx; return 0; }
int   term_has_color(void *ctx)                            { (void)ctx; return 0; }
void  term_ring_bell(void *ctx)                            { (void)ctx; }
void  term_set_fg(void *ctx, term_color_t color)           { (void)ctx; (void)color; }
void  term_set_bg(void *ctx, term_color_t bg, term_color_t fg) { (void)ctx; (void)bg; (void)fg; }
