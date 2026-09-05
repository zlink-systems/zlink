// Public C-API repro of the M6A fanout heap corruption.
// One persistent PUB; many SUBs repeatedly connect/close against it while the
// PUB keeps publishing beacons (like raw_fanout_publisher_t::tick). The SUB
// pipe attach/terminate churn on the PUB's XPUB dist array races with publish.
#include <zlink.h>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <atomic>
#include <vector>
static void publish_once (void *pub) {
    zlink_msg_t m; zlink_msg_init_size (&m, 4);
    memcpy (zlink_msg_data (&m), "beac", 4);
    zlink_publish_part (pub, "topic", &m, ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_FINAL);
    zlink_msg_close (&m);
}
int main (int argc, char **argv) {
    const int iterations = argc > 1 ? atoi (argv[1]) : 400;
    const int concurrent = argc > 2 ? atoi (argv[2]) : 3;
    void *pub_ctx = zlink_ctx_new (); assert (pub_ctx);
    void *pub = zlink_socket (pub_ctx, ZLINK_SOCKET_PUB); assert (pub);
    int zero = 0; assert (zlink_set_option (pub, ZLINK_OPT_LINGER, &zero, sizeof zero) == 0);
    assert (zlink_bind (pub, "tcp://127.0.0.1:0") == 0);
    char endpoint[256]; size_t len = sizeof endpoint;
    assert (zlink_get_option (pub, ZLINK_OPT_LAST_ENDPOINT, endpoint, &len) == 0);
    std::atomic<bool> stop{false};
    std::thread pub_thread ([&]{ while (!stop.load ()) { publish_once (pub); } });
    void *sub_ctx = zlink_ctx_new (); assert (sub_ctx);
    for (int i = 0; i < iterations; ++i) {
        std::vector<void*> subs;
        for (int c = 0; c < concurrent; ++c) {
            void *sub = zlink_socket (sub_ctx, ZLINK_SOCKET_SUB); assert (sub);
            zlink_set_option (sub, ZLINK_OPT_LINGER, &zero, sizeof zero);
            zlink_set_subscription (sub, "");
            zlink_connect (sub, endpoint);
            subs.push_back (sub);
        }
        for (void *sub : subs) zlink_close (sub);
        if ((i + 1) % 100 == 0) { printf ("iter %d ok\n", i + 1); fflush (stdout); }
    }
    stop.store (true); pub_thread.join ();
    zlink_close (pub); zlink_ctx_term (pub_ctx); zlink_ctx_term (sub_ctx);
    printf ("done\n");
    return 0;
}
