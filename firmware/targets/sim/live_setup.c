/**
 * live_setup.c — see live_setup.h.
 */
#include "live_setup.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* 256 KiB read budget for --pack: generous for any real festpack.json
 * (schedules included), same "fixed, documented budget" discipline as
 * fixture.c's FIX_MAX_JSON_LEN — and the same budget the retired live.c's
 * ff_live_load_pack enforced. ff_shell_load_pack takes bytes, not a path
 * (the device has no filesystem the way the sim does), so reading the
 * file under a stated budget is this target's job. */
#define FF_LIVE_SETUP_PACK_MAX (256 * 1024)

/* Splits "HOST:PORT" (last ':' is the separator, so an IPv6 literal host
 * isn't supported — meshtasticd's client API is addressed by hostname or
 * IPv4 in every deployment this repo targets: firmware/tools/dev/
 * compose.yml's service name, or 127.0.0.1). Returns true and fills
 * *host_out (truncated to host_sz) and *port_out on success. */
static bool live_setup_parse_host_port(char const *hostport, char *host_out, size_t host_sz, uint16_t *port_out)
{
    char const *colon = strrchr(hostport, ':');
    if (colon == NULL || colon == hostport || colon[1] == '\0') return false;

    size_t host_len = (size_t)(colon - hostport);
    if (host_len >= host_sz) return false;
    memcpy(host_out, hostport, host_len);
    host_out[host_len] = '\0';

    char *end = NULL;
    long port = strtol(colon + 1, &end, 10);
    if (end == colon + 1 || *end != '\0' || port <= 0 || port > 65535) return false;

    *port_out = (uint16_t)port;
    return true;
}

/* Reads `path` (under FF_LIVE_SETUP_PACK_MAX) and parses it into
 * shell_cfg->pack via ff_shell_load_pack. Returns 0 on success, negative
 * on I/O failure, over-budget, or a parse error. */
static int live_setup_load_pack_file(ff_shell_t *shell, char const *path)
{
    if (path == NULL) return -1;

    FILE *f = fopen(path, "rb");
    if (f == NULL) return -1;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }
    long sz = ftell(f);
    if (sz < 0 || sz > FF_LIVE_SETUP_PACK_MAX) {
        fclose(f);
        return -1;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return -1;
    }

    char *buf = malloc((size_t)sz > 0 ? (size_t)sz : 1);
    if (buf == NULL) {
        fclose(f);
        return -1;
    }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (n != (size_t)sz) {
        free(buf);
        return -1;
    }

    int rc = ff_shell_load_pack(shell, buf, n);
    free(buf);
    return rc;
}

int ff_live_setup(ff_shell_t *shell, ff_shell_cfg_t *shell_cfg, ff_live_setup_cfg_t const *cfg,
                   ff_live_setup_t *out)
{
    if (shell == NULL || shell_cfg == NULL || cfg == NULL || out == NULL) return -1;

    memset(out, 0, sizeof(*out));
    out->tcp.fd = -1;

    if (cfg->connect_hostport != NULL) {
        char host[256];
        uint16_t port;
        if (!live_setup_parse_host_port(cfg->connect_hostport, host, sizeof(host), &port)) {
            fprintf(stderr, "ffsim: --connect expects HOST:PORT, got \"%s\"\n", cfg->connect_hostport);
            return -1;
        }
        if (mc_tcp_open(&out->tcp, host, port) != 0) {
            fprintf(stderr, "ffsim: failed to connect to %s:%u\n", host, (unsigned)port);
            return -1;
        }
        out->tcp_open = true;
        shell_cfg->transport = mc_tcp_transport(&out->tcp);
        printf("ffsim: connected to %s:%u\n", host, (unsigned)port);
    }

    if (ff_shell_init(shell, shell_cfg) != 0) {
        fprintf(stderr, "ffsim: ff_shell_init failed\n");
        ff_live_setup_close(out);
        return -1;
    }

    if (cfg->dev_trust_all) {
        /* S16 AC6's compile-time assertion: this affordance must not
         * exist in a non-sim build. targets/sim only ever builds with
         * FF_TARGET=sim, so this can only fire if the build system stops
         * defining FF_TARGET_SIM — in which case the flag would parse
         * while the shell-side branch was silently compiled away, which
         * is exactly the failure this makes loud. */
#if !defined(FF_TARGET_SIM)
#error "--dev-trust-all is sim-only (S16 AC6): FF_TARGET_SIM must be defined for every build of this file"
#else
        ff_shell_dev_trust_all(shell, true);
        /* The host genuinely knows what time it is (see
         * ff_shell_dev_wall_observe's header comment for why this is
         * honest and why the single-node dev daemon needs it), and the
         * observation is paired with the CURRENT tick source reading —
         * under --mock-clock, later `clock` commands advance the derived
         * wall time from here. */
        (void)ff_shell_dev_wall_observe(shell, (int64_t)time(NULL)); /* a rejected HOST clock leaves wall UNKNOWN — honest */
        out->wall_host_observed = true; /* stated in the ctl `state` dump's wall object */
        printf("ffsim: --dev-trust-all: auto-pairing every NodeInfo sender, treating the daemon's "
               "own node as inbound, and latching the wall clock from the host clock "
               "(sim-only dev affordance, compiled out of device builds — S16 AC6)\n");
#endif
    }

    if (cfg->pack_path != NULL) {
        if (live_setup_load_pack_file(shell, cfg->pack_path) != 0) {
            fprintf(stderr, "ffsim: failed to load festpack %s\n", cfg->pack_path);
            ff_live_setup_close(out);
            return -1;
        }
        /* The shell deliberately does NOT adopt a pack's venue origin as
         * "my position" (the venue centre is not where the wearer is
         * standing — see ff_shell_load_pack's doc). This dev target DOES
         * want the old live.c behaviour for the radar dev loop, so it
         * does it itself, visibly, exactly as that doc prescribes. */
        fp_pack_t const *p = shell_cfg->pack;
        if (p != NULL && p->origin_known) {
            ff_shell_set_my_pos(shell, p->origin);
            printf("ffsim: --pack: adopting the pack's venue origin (%.6f, %.6f) as my position "
                   "(dev affordance — the sim has no GPS)\n",
                   p->origin.lat, p->origin.lon);
        }
    }

    /* Fixed north heading placeholder — the sim has no compass, same
     * documented stand-in the retired live.c used. Without it the radar
     * honestly reports arrow_valid=false for every member, which is
     * correct on device but makes the dev loop's radar face useless. */
    ff_shell_set_heading(shell, 0.0f);

    return 0;
}

void ff_live_setup_close(ff_live_setup_t *out)
{
    if (out == NULL) return;
    if (out->tcp_open) {
        mc_tcp_close(&out->tcp);
        out->tcp_open = false;
    }
}
