/*
 * corebridge.c - a C stand-in for the Swift CoreBridge (PLAN.md 4.2).
 *
 * WP-01 has no Swift toolchain available (swift.org is blocked by the egress
 * proxy), so the three @_cdecl entry points are implemented here in C with the
 * exact signatures the Swift side would export. Two fake services:
 *
 *   metrics  - {"cpu":<0..100>,"history":[<60 samples>]} pushed every 500 ms
 *   settings - {"toggle":<bool>,"slider":<0..100>,"shortcut":"<chord>",
 *               "selection":"<x,y,w,h>"} pushed on every accepted command
 *
 * Commands accepted by `settings`:
 *   {"set":{"key":"toggle","value":true}}
 *   {"set":{"key":"slider","value":42}}
 *   {"set":{"key":"selection","value":"10,20,300,200"}}
 *   {"recordShortcut":"Ctrl+Alt+K"}
 *
 * The JSON parsing here is deliberately a minimal scanner: the real bridge
 * decodes a Codable command enum on the Swift side. What the shell sees --
 * the wire format and the C ABI -- is identical either way.
 */
#define _GNU_SOURCE
#include "corebridge.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define VS_HISTORY 60
#define VS_MAX_SUBS 16

typedef struct {
    vs_snapshot_callback cb;
    void *ctx;
} vs_sub;

typedef struct {
    const char *name;
    vs_sub subs[VS_MAX_SUBS];
    int sub_count;
} vs_service;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static vs_service g_metrics = { "metrics", {{0, 0}}, 0 };
static vs_service g_settings = { "settings", {{0, 0}}, 0 };

/* metrics state */
static double g_history[VS_HISTORY];
static int g_history_len;
static double g_cpu;

/* settings state */
static int g_toggle;
static double g_slider = 35.0;
static char g_shortcut[64] = "none";
static char g_selection[64] = "none";

static pthread_t g_ticker;
static int g_ticker_started;
static volatile int g_stop;

static vs_service *service_for(const char *name)
{
    if (!name) return NULL;
    if (strcmp(name, "metrics") == 0) return &g_metrics;
    if (strcmp(name, "settings") == 0) return &g_settings;
    return NULL;
}

/* -- snapshot rendering (caller holds g_lock) ---------------------------- */

static char *render_metrics(void)
{
    size_t cap = 64 + VS_HISTORY * 8;
    char *out = (char *)malloc(cap);
    if (!out) return NULL;
    int n = snprintf(out, cap, "{\"cpu\":%.2f,\"history\":[", g_cpu);
    for (int i = 0; i < g_history_len; i++)
        n += snprintf(out + n, cap - (size_t)n, "%s%.2f", i ? "," : "", g_history[i]);
    snprintf(out + n, cap - (size_t)n, "]}");
    return out;
}

static char *render_settings(void)
{
    size_t cap = 256;
    char *out = (char *)malloc(cap);
    if (!out) return NULL;
    snprintf(out, cap,
             "{\"toggle\":%s,\"slider\":%.2f,\"shortcut\":\"%s\",\"selection\":\"%s\"}",
             g_toggle ? "true" : "false", g_slider, g_shortcut, g_selection);
    return out;
}

static char *render(const vs_service *svc)
{
    return svc == &g_metrics ? render_metrics() : render_settings();
}

/* Deliver to every subscriber. Caller holds g_lock; the snapshot pointer is
 * only valid for the duration of the callback, matching the header contract. */
static void publish(vs_service *svc)
{
    char *json = render(svc);
    if (!json) return;
    for (int i = 0; i < svc->sub_count; i++)
        svc->subs[i].cb(json, svc->subs[i].ctx);
    free(json);
}

/* -- the fake CPU series ------------------------------------------------- */

static void *ticker(void *unused)
{
    (void)unused;
    unsigned int seed = 0x5eed;
    double phase = 0.0;
    while (!g_stop) {
        struct timespec ts = { 0, 500 * 1000 * 1000 };
        nanosleep(&ts, NULL);
        phase += 0.21;
        /* A slow sine plus bounded noise: readable as a sparkline, and
         * deterministic enough that a screenshot is reproducible in shape. */
        double v = 45.0 + 28.0 * sin(phase) + 12.0 * sin(phase * 2.7)
                 + ((double)rand_r(&seed) / RAND_MAX - 0.5) * 8.0;
        if (v < 0.0) v = 0.0;
        if (v > 100.0) v = 100.0;

        pthread_mutex_lock(&g_lock);
        g_cpu = v;
        if (g_history_len < VS_HISTORY) {
            g_history[g_history_len++] = v;
        } else {
            memmove(g_history, g_history + 1, sizeof(double) * (VS_HISTORY - 1));
            g_history[VS_HISTORY - 1] = v;
        }
        publish(&g_metrics);
        pthread_mutex_unlock(&g_lock);
    }
    return NULL;
}

static void ensure_ticker(void)
{
    if (g_ticker_started) return;
    g_ticker_started = 1;
    pthread_create(&g_ticker, NULL, ticker, NULL);
}

/* -- minimal JSON scanning ----------------------------------------------- */

/* Copy the string value of "key":"..." into out. Returns 1 on success. */
static int json_string(const char *json, const char *key, char *out, size_t cap)
{
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return 0;
    p = strchr(p + strlen(pat), ':');
    if (!p) return 0;
    p = strchr(p, '"');
    if (!p) return 0;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < cap) out[i++] = *p++;
    out[i] = '\0';
    return 1;
}

int vs_subscribe(const char *service, vs_snapshot_callback callback, void *ctx)
{
    vs_service *svc = service_for(service);
    if (!svc || !callback) return -1;

    pthread_mutex_lock(&g_lock);
    if (svc->sub_count >= VS_MAX_SUBS) {
        pthread_mutex_unlock(&g_lock);
        return -1;
    }
    int token = ++svc->sub_count;
    svc->subs[token - 1].cb = callback;
    svc->subs[token - 1].ctx = ctx;
    ensure_ticker();
    /* Fire once immediately so the view has state before the first tick. */
    char *json = render(svc);
    if (json) {
        callback(json, ctx);
        free(json);
    }
    pthread_mutex_unlock(&g_lock);
    return token;
}

int vs_command(const char *service, const char *json)
{
    vs_service *svc = service_for(service);
    if (!svc || !json) return -1;
    if (svc != &g_settings) return -2;

    int handled = 0;
    pthread_mutex_lock(&g_lock);

    if (strstr(json, "\"recordShortcut\"")) {
        char chord[64];
        if (json_string(json, "recordShortcut", chord, sizeof chord)) {
            snprintf(g_shortcut, sizeof g_shortcut, "%s", chord);
            handled = 1;
        }
    } else if (strstr(json, "\"set\"")) {
        char key[32];
        if (json_string(json, "key", key, sizeof key)) {
            const char *vp = strstr(json, "\"value\"");
            if (vp) vp = strchr(vp + 7, ':');
            if (vp) {
                vp++;
                while (*vp == ' ') vp++;
                if (strcmp(key, "toggle") == 0) {
                    g_toggle = (strncmp(vp, "true", 4) == 0);
                    handled = 1;
                } else if (strcmp(key, "slider") == 0) {
                    g_slider = atof(vp);
                    handled = 1;
                } else if (strcmp(key, "selection") == 0) {
                    char sel[64];
                    if (json_string(json, "value", sel, sizeof sel)) {
                        snprintf(g_selection, sizeof g_selection, "%s", sel);
                        handled = 1;
                    }
                }
            }
        }
    }

    if (handled) publish(&g_settings);
    pthread_mutex_unlock(&g_lock);
    return handled ? 0 : -2;
}

char *vs_snapshot(const char *service)
{
    vs_service *svc = service_for(service);
    if (!svc) return NULL;
    pthread_mutex_lock(&g_lock);
    ensure_ticker();
    char *json = render(svc);
    pthread_mutex_unlock(&g_lock);
    return json;
}

void vs_free(char *s)
{
    free(s);
}
