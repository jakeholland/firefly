/**
 * test_transport_tcp.c — S16 slice e: mc_tcp re-dials a dropped
 * connection (docs/specs/S16-app-shell.md slice e; the pre-existing gap
 * flagged in PR #56 — "mc_tcp can't re-dial a dead socket, so
 * mc_client's auto-reconnect can't survive a force-close").
 *
 * A real POSIX loopback listener, deliberately: the bug this closes is a
 * TCP-level fact (a broken fd does not heal itself; nothing but a fresh
 * connect() produces a usable socket again), and `mc_client.c`'s own
 * tests all drive mock read/write callbacks that have no fd to break in
 * the first place — this is the one file positioned to catch a
 * transport that reports "reconnected" without ever having redialed.
 *
 * THE PROXY, stated up front (docs/review/code-review.md item 6): "the
 * write after the drop returned success" is satisfiable by a transport
 * that just quietly returns 0/success against nothing. The property is
 * that a NEW socket was actually opened — pinned here by having the
 * listener `accept()` a second connection and reading the bytes the
 * write claimed to send off of THAT fd, not the original one (which
 * stays dead throughout).
 */
#include <string.h>
#include <time.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "unity.h"

#include "mc_transport_tcp.h"

static int g_listen_fd = -1;
static uint16_t g_listen_port = 0;

static void open_listener(void)
{
    g_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    TEST_ASSERT_TRUE(g_listen_fd >= 0);

    int yes = 1;
    (void)setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0; /* let the kernel pick an ephemeral port */
    TEST_ASSERT_EQUAL_INT(0, bind(g_listen_fd, (struct sockaddr *)&addr, sizeof(addr)));
    TEST_ASSERT_EQUAL_INT(0, listen(g_listen_fd, 4));

    socklen_t alen = sizeof(addr);
    TEST_ASSERT_EQUAL_INT(0, getsockname(g_listen_fd, (struct sockaddr *)&addr, &alen));
    g_listen_port = ntohs(addr.sin_port);
}

void setUp(void)
{
    open_listener();
}

void tearDown(void)
{
    if (g_listen_fd >= 0) {
        close(g_listen_fd);
        g_listen_fd = -1;
    }
}

/* Poll transport->read() until it reports the drop (-1) or a bounded
 * number of attempts elapse — a FIN arriving over loopback is fast but
 * not synchronous with close(), so a single unconditional read() right
 * after closing the peer would be a flaky test. Fails the test outright
 * (rather than returning false) on an unexpected non-zero/non-error
 * read, since that would mean phantom data appeared from nowhere. */
static void wait_for_drop(mc_transport_t *transport)
{
    for (int i = 0; i < 200; i++) {
        uint8_t buf[64];
        int rc = transport->read(transport->io, buf, sizeof(buf));
        if (rc == -1) {
            return; /* the drop was reported */
        }
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, rc, "read() returned unexpected data instead of an error or 'nothing yet'");
        struct timespec ts = {0, 2 * 1000 * 1000}; /* 2 ms */
        nanosleep(&ts, NULL);
    }
    TEST_FAIL_MESSAGE("transport never reported the peer's close");
}

static void S16_e_mc_tcp_open_connects_and_records_the_target(void)
{
    mc_tcp_t t;
    TEST_ASSERT_EQUAL_INT(0, mc_tcp_open(&t, "127.0.0.1", g_listen_port));
    TEST_ASSERT_TRUE(t.fd >= 0);
    TEST_ASSERT_TRUE(t.have_target);
    TEST_ASSERT_EQUAL_STRING("127.0.0.1", t.host);
    TEST_ASSERT_EQUAL_UINT16(g_listen_port, t.port);

    int server_fd = accept(g_listen_fd, NULL, NULL);
    TEST_ASSERT_TRUE(server_fd >= 0);

    close(server_fd);
    mc_tcp_close(&t);
    TEST_ASSERT_TRUE(t.fd < 0);
    TEST_ASSERT_FALSE(t.have_target);
}

/**
 * THE regression this slice fixes. Before it, mc_tcp_t remembered
 * nothing about how it connected, so a write() after a drop kept trying
 * (and failing) against the same dead fd forever — mc_client's own
 * auto-reconnect scheduling was fine, but had no way to ever actually
 * re-establish the socket underneath it.
 */
static void S16_e_transport_redials_after_a_force_close(void)
{
    mc_tcp_t t;
    TEST_ASSERT_EQUAL_INT(0, mc_tcp_open(&t, "127.0.0.1", g_listen_port));

    int server_fd = accept(g_listen_fd, NULL, NULL);
    TEST_ASSERT_TRUE(server_fd >= 0);

    /* THE FORCE-CLOSE: the daemon side goes away with no warning. */
    close(server_fd);

    mc_transport_t transport = mc_tcp_transport(&t);

    /* The drop is reported exactly once ... */
    wait_for_drop(&transport);

    /* ... and NOT on every subsequent call — a repeating -1 is precisely
     * what defeated mc_client's own reconnect backoff before this fix:
     * every read() would reschedule reconnect_at_ms another 2 s out, so
     * the backoff window could never elapse and mc_begin_handshake was
     * never reached. "Nothing available", not "another error". */
    for (int i = 0; i < 5; i++) {
        uint8_t buf[64];
        TEST_ASSERT_EQUAL_INT(0, transport.read(transport.io, buf, sizeof(buf)));
    }

    /* THE FIX: a write() — mc_begin_handshake's exact shape on the real
     * reconnect schedule — transparently re-dials instead of failing
     * against the dead fd forever. */
    uint8_t const payload[] = {0xAA, 0xBB, 0xCC, 0xDD};
    int wrc = transport.write(transport.io, payload, sizeof(payload));
    TEST_ASSERT_EQUAL_INT((int)sizeof(payload), wrc);

    /* Proof it is a REAL new socket, not a write() that merely claims
     * success: the listener has a fresh connection to accept, and the
     * bytes just written arrive on THAT fd (the original server_fd is
     * long gone). */
    int new_server_fd = accept(g_listen_fd, NULL, NULL);
    TEST_ASSERT_TRUE(new_server_fd >= 0);

    uint8_t rx[16];
    ssize_t n = recv(new_server_fd, rx, sizeof(rx), 0);
    TEST_ASSERT_EQUAL_INT((int)sizeof(payload), (int)n);
    TEST_ASSERT_EQUAL_MEMORY(payload, rx, sizeof(payload));

    close(new_server_fd);
    mc_tcp_close(&t);
}

/* A redial that has nowhere to go must fail honestly — not silently
 * succeed, and not spin/hang the caller. */
static void S16_e_redial_fails_honestly_when_nothing_is_listening(void)
{
    mc_tcp_t t;
    TEST_ASSERT_EQUAL_INT(0, mc_tcp_open(&t, "127.0.0.1", g_listen_port));

    int server_fd = accept(g_listen_fd, NULL, NULL);
    TEST_ASSERT_TRUE(server_fd >= 0);
    close(server_fd);

    /* Nobody home any more. */
    close(g_listen_fd);
    g_listen_fd = -1;

    mc_transport_t transport = mc_tcp_transport(&t);
    wait_for_drop(&transport);

    uint8_t const payload[] = {0x01};
    int wrc = transport.write(transport.io, payload, sizeof(payload));
    TEST_ASSERT_LESS_THAN_INT(0, wrc);

    mc_tcp_close(&t);
}

/* An explicit close forgets the target: a stray read()/write() afterward
 * must not silently reopen a socket the caller believes is shut. */
static void S16_e_explicit_close_does_not_redial(void)
{
    mc_tcp_t t;
    TEST_ASSERT_EQUAL_INT(0, mc_tcp_open(&t, "127.0.0.1", g_listen_port));

    int server_fd = accept(g_listen_fd, NULL, NULL);
    TEST_ASSERT_TRUE(server_fd >= 0);
    close(server_fd);

    mc_tcp_close(&t);
    TEST_ASSERT_FALSE(t.have_target);

    mc_transport_t transport = mc_tcp_transport(&t);
    uint8_t const payload[] = {0x02};
    int wrc = transport.write(transport.io, payload, sizeof(payload));
    TEST_ASSERT_LESS_THAN_INT(0, wrc); /* no target recorded -> no redial, honest failure */

    /* Confirm no phantom connection was opened. */
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(g_listen_fd, &rfds);
    struct timeval tv = {.tv_sec = 0, .tv_usec = 20 * 1000};
    int r = select(g_listen_fd + 1, &rfds, NULL, NULL, &tv);
    TEST_ASSERT_EQUAL_INT(0, r); /* nothing pending to accept */
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(S16_e_mc_tcp_open_connects_and_records_the_target);
    RUN_TEST(S16_e_transport_redials_after_a_force_close);
    RUN_TEST(S16_e_redial_fails_honestly_when_nothing_is_listening);
    RUN_TEST(S16_e_explicit_close_does_not_redial);

    return UNITY_END();
}
