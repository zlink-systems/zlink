/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include "native_call_counter.hpp"
#include <zlink.h>
namespace
{
void count (std::size_t index)
{
    if (zlink::framework::tests::count_native_calls.load ()) {
        if (index == 4) {
            std::lock_guard lock (zlink::framework::tests::native_close_mutex);
            zlink::framework::tests::native_calls[index].fetch_add (1);
        } else
            zlink::framework::tests::native_calls[index].fetch_add (1);
    }
}
}
extern "C" {
zlink_config_result_t __real_zlink_msg_init (zlink_msg_t *);
zlink_config_result_t __real_zlink_msg_init_size (zlink_msg_t *, size_t);
zlink_config_result_t
__real_zlink_msg_init_data (zlink_msg_t *, void *, size_t, zlink_free_fn *, void *);
zlink_config_result_t __real_zlink_msg_copy (zlink_msg_t *, zlink_msg_t *);
zlink_config_result_t __real_zlink_msg_close (zlink_msg_t *);
int __real_zlink_poller_wait (void *, zlink_poller_event_t *, int, long, zlink_config_result_t *);
zlink_config_result_t __wrap_zlink_msg_init (zlink_msg_t *m)
{
    count (0);
    return __real_zlink_msg_init (m);
}
zlink_config_result_t __wrap_zlink_msg_init_size (zlink_msg_t *m, size_t s)
{
    count (1);
    return __real_zlink_msg_init_size (m, s);
}
zlink_config_result_t
__wrap_zlink_msg_init_data (zlink_msg_t *m, void *p, size_t s, zlink_free_fn *f, void *h)
{
    count (2);
    return __real_zlink_msg_init_data (m, p, s, f, h);
}
zlink_config_result_t __wrap_zlink_msg_copy (zlink_msg_t *a, zlink_msg_t *b)
{
    count (3);
    return __real_zlink_msg_copy (a, b);
}
zlink_config_result_t __wrap_zlink_msg_close (zlink_msg_t *m)
{
    count (4);
    const auto result = __real_zlink_msg_close (m);
    zlink::framework::tests::native_close_changed.notify_all ();
    return result;
}
int __wrap_zlink_poller_wait (
  void *p, zlink_poller_event_t *e, int n, long t, zlink_config_result_t *err)
{
    count (5);
    const auto result = __real_zlink_poller_wait (p, e, n, t, err);
    if (zlink::framework::tests::count_native_calls.load ())
        for (int index = 0; index < result; ++index)
            if ((e[index].events & ZLINK_POLLCOMPLETION) != 0)
                zlink::framework::tests::native_completion_events.fetch_add (1);
    return result;
}
}
