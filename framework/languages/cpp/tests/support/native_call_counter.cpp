/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include "native_call_counter.hpp"
#include <zlink.h>
#include <unordered_map>
namespace
{
using namespace zlink::framework::tests;
native_message_filter_t message_filter = nullptr;
enum class native_initialization_t
{
    unmeasured,
    measured,
    excluded
};
std::unordered_map<zlink_msg_t *, native_initialization_t> initializations;

bool excluded (zlink_msg_t *message)
{
    return message_filter && message_filter (zlink_msg_data (message), zlink_msg_size (message));
}

bool count (std::size_t index)
{
    const bool enabled = count_native_calls.load ();
    if (enabled)
        native_calls[index].fetch_add (1);
    return enabled;
}
}

namespace zlink::framework::tests
{
void set_native_message_filter (native_message_filter_t filter)
{
    std::lock_guard lock (native_close_mutex);
    message_filter = filter;
    initializations.clear ();
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
    const auto result = __real_zlink_msg_init (m);
    std::lock_guard lock (native_close_mutex);
    const bool counted = count (0);
    if (message_filter)
        initializations[m] =
          counted ? native_initialization_t::measured : native_initialization_t::unmeasured;
    return result;
}
zlink_config_result_t __wrap_zlink_msg_init_size (zlink_msg_t *m, size_t s)
{
    {
        std::lock_guard lock (native_close_mutex);
        initializations.erase (m);
        count (1);
    }
    return __real_zlink_msg_init_size (m, s);
}
zlink_config_result_t
__wrap_zlink_msg_init_data (zlink_msg_t *m, void *p, size_t s, zlink_free_fn *f, void *h)
{
    {
        std::lock_guard lock (native_close_mutex);
        initializations.erase (m);
        if (!message_filter || !message_filter (p, s))
            count (2);
    }
    return __real_zlink_msg_init_data (m, p, s, f, h);
}
zlink_config_result_t __wrap_zlink_msg_copy (zlink_msg_t *a, zlink_msg_t *b)
{
    {
        std::lock_guard lock (native_close_mutex);
        if (excluded (b)) {
            // A borrowed native view is initialized before its payload is known.
            // Classify that actual initializer together with the copy; Core may
            // consume the view before close, so preserve the classification.
            auto &initialization = initializations[a];
            if (initialization == native_initialization_t::measured)
                native_calls[0].fetch_sub (1);
            initialization = native_initialization_t::excluded;
        } else
            count (3);
    }
    return __real_zlink_msg_copy (a, b);
}
zlink_config_result_t __wrap_zlink_msg_close (zlink_msg_t *m)
{
    {
        std::lock_guard lock (native_close_mutex);
        const auto found = initializations.find (m);
        const bool excluded_view =
          found != initializations.end () && found->second == native_initialization_t::excluded;
        if (!excluded_view && !excluded (m))
            count (4);
        if (found != initializations.end ())
            initializations.erase (found);
    }
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
