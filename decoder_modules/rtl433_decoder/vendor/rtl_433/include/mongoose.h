/*
    Predator RF — minimal mongoose.h stub.

    The full mongoose embedded networking library is not used in the Predator
    Android build. We don't ship rtl_433's HTTP server, MQTT/InfluxDB outputs,
    or its GPSD client — Predator handles networking and location through the
    Android stack and through its own bridge ingesters.

    This stub provides just enough type and symbol surface to keep the
    retained rtl_433 sources (data_tag.c, r_api.c) compiling and linking.
    All function bodies live in src/predator_stubs.c and are no-ops.
*/
#ifndef PREDATOR_STUB_MONGOOSE_H
#define PREDATOR_STUB_MONGOOSE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Event flags + connection flags used by the gpsd block in data_tag.c. */
#define MG_F_CLOSE_IMMEDIATELY 0x40

#define MG_EV_POLL    0
#define MG_EV_CONNECT 1
#define MG_EV_RECV    2
#define MG_EV_CLOSE   3

/* Minimal mbuf — the gpsd code only touches buf/len. */
struct mbuf {
    char  *buf;
    size_t len;
    size_t size;
};

struct mg_mgr {
    void *opaque;
};

struct mg_connection;

typedef void (*mg_event_handler_t)(struct mg_connection *nc, int ev, void *ev_data);

struct mg_connection {
    void              *user_data;
    struct mg_mgr     *mgr;
    struct mbuf        recv_mbuf;
    struct mbuf        send_mbuf;
    int                sock;
    unsigned long      flags;
    mg_event_handler_t handler;
    struct mg_connection *next;
};

struct mg_connect_opts {
    void         *user_data;
    const char  **error_string;
    unsigned int  flags;
};

void                 mg_mgr_init(struct mg_mgr *mgr, void *user_data);
void                 mg_mgr_free(struct mg_mgr *mgr);
void                 mg_send(struct mg_connection *nc, const void *buf, int len);
struct mg_connection *mg_connect_opt(struct mg_mgr *mgr,
                                     const char *address,
                                     mg_event_handler_t handler,
                                     struct mg_connect_opts opts);

/* mbuf API — only mbuf_remove is referenced by the retained sources
   (data_tag.c gpsd block). Stubbed in predator_stubs.c. */
void                 mbuf_remove(struct mbuf *mb, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* PREDATOR_STUB_MONGOOSE_H */
