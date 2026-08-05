#include <string.h>

#include <zlink.h>

#define CHECK(expr)                                                                                \
    do {                                                                                           \
        if (!(expr))                                                                               \
            return __LINE__;                                                                       \
    } while (0)

static int wait_for_poller_event (void *poller, zlink_poller_event_t *event, int timeout_ms)
{
    for (int attempt = 0; attempt < 10; ++attempt) {
        zlink_config_result_t error = ZLINK_CONFIG_OK;
        const int rc = zlink_poller_wait (poller, event, 1, timeout_ms, &error);
        if (rc == 1)
            return 1;
        CHECK (rc == 0);
        CHECK (error == ZLINK_CONFIG_OK);
    }
    return 0;
}

int main (void)
{
    void *ctx = zlink_ctx_new ();
    CHECK (ctx != NULL);

    void *pair = zlink_socket (ctx, ZLINK_SOCKET_PAIR);
    CHECK (pair != NULL);

    zlink_socket_monitor_open_options_t monitor_options;
    memset (&monitor_options, 0, sizeof (monitor_options));
    monitor_options.events = ZLINK_SOCKET_MONITOR_EVENT_ALL;
    void *monitor = zlink_socket_monitor_open (pair, &monitor_options);
    CHECK (monitor != NULL);

    zlink_socket_monitor_event_t event;
    CHECK (zlink_socket_monitor_recv (monitor, &event, ZLINK_RECV_FLAGS_DONTWAIT)
           == ZLINK_RECV_NO_DATA);

    void *poller = zlink_poller_new ();
    CHECK (poller != NULL);
    CHECK (zlink_poller_add (poller, pair, pair, ZLINK_POLLIN) == ZLINK_CONFIG_OK);
    CHECK (zlink_poller_size (poller, NULL) == 1);
    CHECK (zlink_poller_modify (poller, pair, ZLINK_POLLOUT) == ZLINK_CONFIG_OK);
    CHECK (zlink_poller_remove (poller, pair) == ZLINK_CONFIG_OK);
    CHECK (zlink_poller_size (poller, NULL) == 0);

    zlink_poller_event_t invalid_event;
    zlink_config_result_t invalid_error = ZLINK_CONFIG_OK;
    CHECK (zlink_poller_wait (poller, &invalid_event, 0, 0, &invalid_error) == -1);
    CHECK (invalid_error == ZLINK_CONFIG_INVALID_ARGUMENT);

    void *timer = zlink_timer_new ();
    CHECK (timer != NULL);
    int timer_slot = 42;
    CHECK (zlink_poller_add_timer (poller, timer, &timer_slot) == ZLINK_CONFIG_OK);
    CHECK (zlink_poller_size (poller, NULL) == 1);
    CHECK (zlink_timer_start (timer, 20 * 1000 * 1000ULL, 1) == ZLINK_CONFIG_OK);

    zlink_poller_event_t timer_event;
    memset (&timer_event, 0, sizeof timer_event);
    CHECK (wait_for_poller_event (poller, &timer_event, 100) == 1);
    CHECK (timer_event.source_kind == ZLINK_POLLER_SOURCE_TIMER);
    CHECK (timer_event.socket == NULL);
    CHECK (timer_event.timer == timer);
    CHECK (timer_event.user_data == &timer_slot);
    CHECK (timer_event.events == ZLINK_POLLIN);

    uint64_t fire_count = 0;
    CHECK (zlink_timer_recv (timer, &fire_count) == ZLINK_RECV_OK);
    CHECK (fire_count == 1);
    CHECK (zlink_poller_remove_timer (poller, timer) == ZLINK_CONFIG_OK);
    CHECK (zlink_timer_stop (timer) == ZLINK_CONFIG_OK);
    CHECK (zlink_timer_destroy (&timer) == ZLINK_CLOSE_OK);
    CHECK (timer == NULL);

    CHECK (zlink_poller_destroy (&poller) == ZLINK_CLOSE_OK);
    CHECK (poller == NULL);

    CHECK (zlink_monitor_close (&monitor) == ZLINK_CLOSE_OK);
    CHECK (monitor == NULL);
    CHECK (zlink_close (pair) == ZLINK_CLOSE_OK);
    CHECK (zlink_ctx_term (ctx) == ZLINK_CLOSE_OK);
    return 0;
}
