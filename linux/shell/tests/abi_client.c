/*
 * abi_client.c - the proof that the C ABI in corebridge.h is real.
 *
 * WP-18. This program includes nothing but linux/shell/include/corebridge.h
 * and links the Swift core built as a static library. If it compiles, the
 * header and the @_cdecl exports agree on names and signatures; if it runs,
 * the four calls do what the header says.
 *
 * It is deliberately C and not C++: a mistake in the header that `extern "C"`
 * would paper over shows up here as a link error.
 *
 * The `bridge-abi` leg of .github/workflows/linux-port-ci.yml builds and runs
 * it. Every check is fatal, so a green run is the evidence and the printed
 * JSON is what a reviewer reads.
 */
#include "corebridge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static void check(int condition, const char *what)
{
    printf("%s %s\n", condition ? "[ ok ]" : "[FAIL]", what);
    if (!condition) failures++;
}

/* Counts the callbacks and keeps the last snapshot, copied -- the header says
 * the pointer dies with the call, so a client that stored it would be the bug
 * this file exists to rule out. */
struct sink {
    int calls;
    char last[65536];
};

static void on_snapshot(const char *json, void *ctx)
{
    struct sink *s = (struct sink *)ctx;
    s->calls++;
    snprintf(s->last, sizeof s->last, "%s", json ? json : "(null)");
}

static char *snapshot_or_die(const char *service)
{
    char *json = vs_snapshot(service);
    if (!json) {
        printf("[FAIL] vs_snapshot(\"%s\") returned NULL\n", service);
        failures++;
    }
    return json;
}

int main(void)
{
    /* ---- vs_snapshot / vs_free ------------------------------------- */

    char *metrics = snapshot_or_die("metrics");
    if (metrics) {
        printf("metrics snapshot: %s\n", metrics);
        check(strstr(metrics, "\"cpu\":") != NULL, "metrics snapshot has cpu");
        check(strstr(metrics, "\"history\":") != NULL, "metrics snapshot has history");
        check(strstr(metrics, "\"capacity\":60") != NULL, "metrics capacity is 60");
        /* Sorted keys: capacity, cpu, history, source. */
        check(strncmp(metrics, "{\"capacity\":", 12) == 0,
              "metrics snapshot keys are sorted");
        vs_free(metrics);
    }

    char *l10n = snapshot_or_die("l10n");
    if (l10n) {
        printf("l10n snapshot: %zu bytes\n", strlen(l10n));
        check(strstr(l10n, "\"language\":") != NULL, "l10n snapshot has language");
        check(strstr(l10n, "\"menuQuit\":") != NULL, "l10n snapshot carries the catalog");
        check(strlen(l10n) > 10000, "the catalog is the whole catalog");
        vs_free(l10n);
    }

    char *features = snapshot_or_die("featureRuntime");
    if (features) {
        printf("featureRuntime snapshot: %s\n", features);
        check(strstr(features, "\"switcher\"") != NULL,
              "featureRuntime snapshot lists features");
        vs_free(features);
    }

    check(vs_snapshot("nosuchservice") == NULL, "vs_snapshot of an unknown service is NULL");

    /* A thousand round trips: a leak here is a leak in every panel refresh. */
    for (int i = 0; i < 1000; i++) {
        char *s = vs_snapshot("metrics");
        if (!s) { check(0, "vs_snapshot in the allocation loop"); break; }
        vs_free(s);
    }
    check(1, "1000 vs_snapshot/vs_free round trips");

    /* ---- vs_subscribe ----------------------------------------------- */

    struct sink sink = { 0, { 0 } };
    int token = vs_subscribe("metrics", on_snapshot, &sink);
    check(token >= 1, "vs_subscribe returns a token >= 1");
    check(sink.calls == 1, "vs_subscribe fires once immediately");
    check(vs_subscribe("nosuchservice", on_snapshot, &sink) == -1,
          "vs_subscribe of an unknown service is -1");
    check(vs_subscribe("metrics", NULL, NULL) == -1,
          "a NULL callback is refused, not dereferenced");

    /* ---- vs_command -------------------------------------------------- */

    check(vs_command("metrics", "{\"sample\":42.5}") == 0, "vs_command accepted");
    check(sink.calls == 2, "the accepted command pushed a snapshot");
    printf("after sample: %s\n", sink.last);
    check(strstr(sink.last, "42.5") != NULL, "the new sample is in the snapshot");

    check(vs_command("metrics", "{\"sample\":42.5}") == 0, "the same sample again is accepted");
    check(sink.calls == 3, "…and appending it is a real change, so it is delivered");

    check(vs_command("nosuchservice", "{}") == -1, "unknown service is -1");
    check(vs_command("metrics", "not json at all") == -3, "malformed JSON is -3");
    check(vs_command("metrics", "{\"noSuchCommand\":1}") == -3, "an unknown command is -3");
    check(vs_command("l10n", "{\"setLanguage\":\"kl\"}") == -2,
          "a command the service refuses is -2");

    check(vs_command("l10n", "{\"setLanguage\":\"pt-BR\"}") == 0, "l10n accepts a language");
    char *after = snapshot_or_die("l10n");
    if (after) {
        check(strstr(after, "\"language\":\"pt-BR\"") != NULL, "the language really changed");
        vs_free(after);
    }

    check(vs_command("featureRuntime", "{\"install\":\"switcher\"}") == 0,
          "featureRuntime accepts an install");
    check(vs_command("featureRuntime", "{\"install\":\"nosuchfeature\"}") == -2,
          "…and refuses one it does not have");

    /* ---- the diff ----------------------------------------------------- */

    int before = sink.calls;
    check(vs_command("metrics", "{\"reset\":true}") == 0, "metrics accepts a reset");
    check(sink.calls == before + 1, "the reset changed the snapshot, so it was delivered");
    check(vs_command("metrics", "{\"reset\":true}") == 0, "a second reset is accepted");
    check(sink.calls == before + 1,
          "…and delivers nothing, because the snapshot did not change");

    printf("%s: %d check(s) failed\n", failures ? "FAILED" : "OK", failures);
    return failures ? 1 : 0;
}
