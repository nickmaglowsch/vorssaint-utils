/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Vorssaint
 *
 * `/proc/net/dev` counters, per-interface rates against this instance's
 * previous call, the default-route interface, and the wifi/ethernet/virtual
 * classification `/sys/class/net` supports.
 */

#include "vs_sensors_internal.h"

#include <stdlib.h>
#include <string.h>

/* ARPHRD_LOOPBACK; /sys/class/net/<if>/type carries the ARP hardware type. */
#define VS_ARPHRD_LOOPBACK 772

static vs_network_kind classify(vs_sensors_impl *impl, const char *name)
{
    char path[VS_FULLPATH_MAX];
    int64_t type = -1;
    if (vs_pathf(impl, path, sizeof path, "/sys/class/net/%s/type", name) &&
        vs_read_i64(path, &type) && type == VS_ARPHRD_LOOPBACK) {
        return VS_NETWORK_LOOPBACK;
    }
    /* A wireless NIC has either the legacy `wireless` directory (wext) or the
     * `phy80211` symlink (cfg80211); every in-tree driver has the latter. */
    if (vs_pathf(impl, path, sizeof path, "/sys/class/net/%s/phy80211", name) &&
        vs_path_exists(path)) {
        return VS_NETWORK_WIFI;
    }
    if (vs_pathf(impl, path, sizeof path, "/sys/class/net/%s/wireless", name) &&
        vs_path_exists(path)) {
        return VS_NETWORK_WIFI;
    }
    /* No `device` link means no hardware behind it: bridge, veth, tun, VPN.
     * `MetricFormat.includeNetworkInterface` excludes the same class on macOS
     * so a VPN does not double-count the physical NIC's traffic. */
    if (vs_pathf(impl, path, sizeof path, "/sys/class/net/%s/device", name) &&
        vs_path_exists(path)) {
        return VS_NETWORK_ETHERNET;
    }
    if (vs_pathf(impl, path, sizeof path, "/sys/class/net/%s", name) && vs_dir_exists(path)) {
        return VS_NETWORK_VIRTUAL;
    }
    return VS_NETWORK_UNKNOWN;
}

static bool is_up(vs_sensors_impl *impl, const char *name)
{
    char path[VS_FULLPATH_MAX];
    char state[32];
    if (!vs_pathf(impl, path, sizeof path, "/sys/class/net/%s/operstate", name)) {
        return false;
    }
    if (!vs_read_text(path, state, sizeof state)) {
        return false;
    }
    return strcmp(vs_trim(state), "up") == 0;
}

/* The IPv4 default route is the /proc/net/route row with destination 0 and the
 * RTF_UP|RTF_GATEWAY flags; IPv6 uses a /0 prefix in /proc/net/ipv6_route. */
static void find_default_routes(vs_sensors_impl *impl, char v4[VS_SENSORS_NAME_MAX],
                                char v6[VS_SENSORS_NAME_MAX])
{
    v4[0] = '\0';
    v6[0] = '\0';

    FILE *file = vs_open_rooted(impl, "/proc/net/route");
    if (file != NULL) {
        char line[512];
        bool first = true;
        while (fgets(line, sizeof line, file) != NULL) {
            if (first) {
                first = false;
                continue;
            }
            char name[VS_SENSORS_NAME_MAX];
            unsigned long destination = 0;
            unsigned long flags = 0;
            if (sscanf(line, "%63s %lx %*s %lx", name, &destination, &flags) == 3 &&
                destination == 0 && (flags & 0x2u) != 0) {
                vs_copy(v4, VS_SENSORS_NAME_MAX, name);
                break;
            }
        }
        fclose(file);
    }

    file = vs_open_rooted(impl, "/proc/net/ipv6_route");
    if (file != NULL) {
        char line[512];
        while (fgets(line, sizeof line, file) != NULL) {
            char destination[64];
            unsigned prefix = 0;
            char name[VS_SENSORS_NAME_MAX];
            /* dest prefix src srcprefix nexthop metric refcnt use flags iface */
            if (sscanf(line, "%63s %x %*s %*x %*s %*x %*x %*x %*x %63s", destination, &prefix,
                       name) != 3) {
                continue;
            }
            if (prefix != 0 || strcmp(name, "lo") == 0) {
                continue;
            }
            if (strspn(destination, "0") != strlen(destination)) {
                continue;
            }
            vs_copy(v6, VS_SENSORS_NAME_MAX, name);
            break;
        }
        fclose(file);
    }
}

static const vs_counter_prev *find_previous(const vs_sensors_impl *impl, const char *name)
{
    for (size_t i = 0; i < impl->prev_net_count; i++) {
        if (strcmp(impl->prev_net[i].name, name) == 0) {
            return &impl->prev_net[i];
        }
    }
    return NULL;
}

int vs_sensors_read_network(vs_sensors_impl *impl, vs_network_sample **out, size_t *count_out)
{
    if (impl == NULL || out == NULL || count_out == NULL) {
        return VS_ERR_INVALID;
    }
    *out = NULL;
    *count_out = 0;

    FILE *file = vs_open_rooted(impl, "/proc/net/dev");
    if (file == NULL) {
        return VS_ERR_BACKEND;
    }

    char v4[VS_SENSORS_NAME_MAX];
    char v6[VS_SENSORS_NAME_MAX];
    find_default_routes(impl, v4, v6);

    double now = vs_now();
    double elapsed = now - impl->prev_net_time;
    bool have_previous = impl->prev_net != NULL && impl->prev_net_count > 0 && elapsed > 0;

    vs_network_sample *samples = NULL;
    size_t count = 0;
    size_t capacity = 0;
    char line[1024];
    int header_lines = 2;
    int result = VS_OK;

    while (fgets(line, sizeof line, file) != NULL) {
        if (header_lines > 0) {
            header_lines--;
            continue;
        }
        char *colon = strchr(line, ':');
        if (colon == NULL) {
            continue;
        }
        *colon = '\0';
        char name[VS_SENSORS_NAME_MAX];
        vs_copy(name, sizeof name, vs_trim(line));
        if (name[0] == '\0') {
            continue;
        }

        unsigned long long f[16];
        memset(f, 0, sizeof f);
        if (sscanf(colon + 1,
                   "%llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
                   &f[0], &f[1], &f[2], &f[3], &f[4], &f[5], &f[6], &f[7], &f[8], &f[9], &f[10],
                   &f[11], &f[12], &f[13], &f[14], &f[15]) < 16) {
            continue;
        }

        if (count == capacity) {
            size_t next = capacity == 0 ? 8 : capacity * 2;
            vs_network_sample *grown = realloc(samples, next * sizeof *grown);
            if (grown == NULL) {
                result = VS_ERR_NO_MEM;
                break;
            }
            samples = grown;
            capacity = next;
        }

        vs_network_sample *sample = &samples[count];
        memset(sample, 0, sizeof *sample);
        vs_copy(sample->name, sizeof sample->name, name);
        sample->rx_bytes = f[0];
        sample->rx_packets = f[1];
        sample->rx_errors = f[2];
        sample->rx_dropped = f[3];
        sample->tx_bytes = f[8];
        sample->tx_packets = f[9];
        sample->tx_errors = f[10];
        sample->tx_dropped = f[11];
        sample->kind = classify(impl, name);
        sample->is_up = is_up(impl, name);
        sample->is_default_route = (v4[0] != '\0' && strcmp(v4, name) == 0) ||
                                   (v6[0] != '\0' && strcmp(v6, name) == 0);

        if (have_previous) {
            const vs_counter_prev *previous = find_previous(impl, name);
            if (previous != NULL) {
                sample->rx_bytes_per_second = vs_rate(previous->a, sample->rx_bytes, elapsed);
                sample->tx_bytes_per_second = vs_rate(previous->b, sample->tx_bytes, elapsed);
                sample->has_rates = true;
            }
        }
        count++;
    }
    fclose(file);

    if (result != VS_OK) {
        free(samples);
        return result;
    }

    vs_counter_prev *next_prev = count > 0 ? calloc(count, sizeof *next_prev) : NULL;
    if (next_prev != NULL) {
        for (size_t i = 0; i < count; i++) {
            vs_copy(next_prev[i].name, sizeof next_prev[i].name, samples[i].name);
            next_prev[i].a = samples[i].rx_bytes;
            next_prev[i].b = samples[i].tx_bytes;
        }
        free(impl->prev_net);
        impl->prev_net = next_prev;
        impl->prev_net_count = count;
        impl->prev_net_time = now;
    }

    *out = samples;
    *count_out = count;
    return VS_OK;
}
