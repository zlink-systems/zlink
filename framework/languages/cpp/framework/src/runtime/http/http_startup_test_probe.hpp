/* SPDX-License-Identifier: Elastic-2.0 */
#pragma once

#if defined(ZLINK_FRAMEWORK_TEST_CREATION_PROBE)
#include <functional>

namespace zlink::framework::runtime
{
enum class http_startup_event_t
{
    before_bind,
    before_seal_listener,
    after_seal_listener,
    listener_destroyed,
    rollback_complete,
    accept_started,
    shutdown_worker_joined
};

// Installed by the fixture before it starts the host threads.
inline std::function<void (http_startup_event_t)> http_startup_test_probe;

inline void probe_http_startup (http_startup_event_t event)
{
    if (http_startup_test_probe)
        http_startup_test_probe (event);
}
} // namespace zlink::framework::runtime
#endif
