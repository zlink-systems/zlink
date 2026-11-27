/* SPDX-License-Identifier: Elastic-2.0 */
#pragma once

#ifdef ZLINK_FRAMEWORK_TEST_CREATION_PROBE
#include <functional>
#include <memory>
#include <utility>

namespace zlink::framework::runtime
{
enum class stream_startup_event_t
{
    before_bind,
    before_drain_listener,
    rollback_complete,
    accept_started
};

// Installed before startup and cleared after the host has joined its workers.
inline std::function<void (stream_startup_event_t, std::weak_ptr<void>)> stream_startup_test_probe;

inline void probe_stream_startup (stream_startup_event_t event, std::weak_ptr<void> listener = {})
{
    if (stream_startup_test_probe)
        stream_startup_test_probe (event, std::move (listener));
}
} // namespace zlink::framework::runtime
#endif
