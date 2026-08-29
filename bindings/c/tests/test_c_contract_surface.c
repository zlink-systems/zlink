#include <string.h>

#include <zlink.h>

#define CHECK(expr)                                                                                \
    do {                                                                                           \
        if (!(expr))                                                                               \
            return __LINE__;                                                                       \
    } while (0)

int main (void)
{
    CHECK (ZLINK_VERSION_MAJOR == 0);
    CHECK (ZLINK_VERSION_MINOR == 14);
    CHECK (ZLINK_VERSION_PATCH == 5);
    CHECK (ZLINK_VERSION == ZLINK_MAKE_VERSION (0, 14, 5));

    CHECK (ZLINK_SOCKET_PAIR == 0x1001);
    CHECK (ZLINK_SOCKET_STREAM == 0x1008);
    CHECK (ZLINK_DONTWAIT == ZLINK_SEND_FLAGS_DONTWAIT);
    CHECK (ZLINK_CTX_OPT_AUTO_HWM_MEMORY_LIMIT_BYTES == 19);
    CHECK (ZLINK_CTX_OPT_AUTO_HWM_RUNTIME_MEMORY_LIMIT_BYTES == 20);
    CHECK (ZLINK_CTX_OPT_AUTO_HWM_CORE_BUDGET_BYTES == 21);
    CHECK (ZLINK_AUTO_HWM_BUDGET_SNAPSHOT_ABI_V1 == 1u);
    CHECK (ZLINK_MONITOR_STATUS_ABI_VERSION == 4u);
    CHECK (sizeof (zlink_socket_monitor_open_options_t) == 16u);

    zlink_msg_t msg;
    CHECK (sizeof msg >= 64);

    int major = 0;
    int minor = 0;
    int patch = 0;
    zlink_version (&major, &minor, &patch);
    CHECK (major == ZLINK_VERSION_MAJOR);
    CHECK (minor == ZLINK_VERSION_MINOR);
    CHECK (patch == ZLINK_VERSION_PATCH);

    CHECK (zlink_send_part != NULL);
    CHECK (zlink_send_part_rid != NULL);
    CHECK (zlink_recv_part != NULL);
    CHECK (zlink_publish_part != NULL);
    CHECK (zlink_subscribe_part != NULL);

    void *ctx = zlink_ctx_new ();
    CHECK (ctx != NULL);
    zlink_auto_hwm_budget_snapshot_t snapshot;
    memset (&snapshot, 0, sizeof (snapshot));
    snapshot.abi_version = ZLINK_AUTO_HWM_BUDGET_SNAPSHOT_ABI_V1;
    snapshot.struct_size = sizeof (snapshot);
    CHECK (zlink_ctx_get_auto_hwm_budget_snapshot (ctx, &snapshot) == ZLINK_CONFIG_OK);
    CHECK (zlink_ctx_reset_auto_hwm_budget_metrics (ctx) == ZLINK_CONFIG_OK);
    CHECK (zlink_ctx_term (ctx) == ZLINK_CLOSE_OK);
    return 0;
}
