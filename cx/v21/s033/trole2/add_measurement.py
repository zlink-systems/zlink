from pathlib import Path
root=Path('/root/worktree/zlink-v21-s033/trole')
base=root/'framework/languages/cpp/framework/src/runtime'
header=base/'dispatch/receive_role_measurement.hpp'
header.write_text('''/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once

// Measurement-only instrumentation. This file is excluded from the product
// commit; ZLINK_PERF_RECEIVE_ROLE_LOG enables the counters and per-record tags.
#include <atomic>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <utility>

namespace zlink::framework::runtime::receive_role_measurement
{
struct rotation_t {};
struct counters_t
{
    const char *path = std::getenv ("ZLINK_PERF_RECEIVE_ROLE_LOG");
    std::atomic_size_t rotations{0}, eligible{0}, attempts{0}, successes{0};
    std::atomic_size_t records_started{0}, records_started_locally{0};
    bool enabled () const noexcept { return path && *path; }
    ~counters_t ()
    {
        if (!enabled ())
            return;
        std::ofstream output (path);
        output << "R\\tE\\tA\\tH\\trecords_started\\trecords_started_locally\\n"
               << rotations.load () << '\\t' << eligible.load () << '\\t'
               << attempts.load () << '\\t' << successes.load () << '\\t'
               << records_started.load () << '\\t' << records_started_locally.load () << '\\n';
    }
};
inline counters_t &counters ()
{
    static counters_t result;
    return result;
}
inline thread_local std::shared_ptr<const rotation_t> receive_rotation;
inline thread_local std::shared_ptr<const rotation_t> local_turn;
inline void begin_rotation ()
{
    if (counters ().enabled ())
        receive_rotation = std::make_shared<rotation_t> ();
}
inline void end_rotation ()
{
    if (receive_rotation)
        counters ().rotations.fetch_add (1, std::memory_order_relaxed);
}
inline void enter_application (bool handed_off)
{
    if (receive_rotation) {
        if (handed_off)
            local_turn = std::move (receive_rotation);
        else
            receive_rotation.reset ();
    }
}
inline void record_started (const std::shared_ptr<const rotation_t> &rotation)
{
    counters ().records_started.fetch_add (1, std::memory_order_relaxed);
    if (rotation == local_turn)
        counters ().records_started_locally.fetch_add (1, std::memory_order_relaxed);
}
} // namespace zlink::framework::runtime::receive_role_measurement
''')
p=base/'dispatch/offload_executor.cpp';s=p.read_text().replace('#include "runtime/dispatch/offload_executor.hpp"','#include "runtime/dispatch/offload_executor.hpp"\n#include "receive_role_measurement.hpp"')
s=s.replace('''    std::unique_lock lock (_mutex, std::try_to_lock);
    if (!lock.owns_lock ())''','''    if (receive_role_measurement::receive_rotation)
        receive_role_measurement::counters ().attempts.fetch_add (1, std::memory_order_relaxed);
    std::unique_lock lock (_mutex, std::try_to_lock);
    if (!lock.owns_lock ())''')
s=s.replace('''    auto *available = current && !receiver ? current : _idle_head;''','''    // E counts rotations with an eligible application queue head after the
    // scheduler try-lock succeeds, before idle availability is considered.
    if (receiver && receive_role_measurement::receive_rotation)
        receive_role_measurement::counters ().eligible.fetch_add (1, std::memory_order_relaxed);
    auto *available = current && !receiver ? current : _idle_head;''')
s=s.replace('''    return assigned;
}''','''    if (receiver && receive_role_measurement::receive_rotation)
        receive_role_measurement::counters ().successes.fetch_add (1, std::memory_order_relaxed);
    return assigned;
}''',1)
s=s.replace('''            if ((*role)->run ()) {''','''            receive_role_measurement::begin_rotation ();
            if ((*role)->run ()) {
                receive_role_measurement::end_rotation ();''',1)
s=s.replace('''                auto successor = try_handoff (worker);
                if (successor)''','''                auto successor = try_handoff (worker);
                receive_role_measurement::enter_application (static_cast<bool> (successor));
                if (successor)''')
s=s.replace('''                continue;
            }
        } else {
            run_application_turn''','''                continue;
            }
            receive_role_measurement::receive_rotation.reset ();
        } else {
            run_application_turn''')
s=s.replace('''        const bool was_role = std::holds_alternative''','''        receive_role_measurement::local_turn.reset ();
        const bool was_role = std::holds_alternative''')
p.write_text(s)
p=base/'mesh/mesh_node_host_service.cpp';s=p.read_text();s=s.replace('/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */', '/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */\n#include "runtime/dispatch/receive_role_measurement.hpp"', 1)
needle='''                  node->application_work_enqueued ();
              },
              std::move (submit_owner),'''
assert s.count(needle)==1
s=s.replace(needle,'''                  // Tag only records accepted during this receive rotation.
                  // Equality with the former receiver's next queue-head turn
                  // distinguishes actual local entry from a successful handoff.
                  if (auto rotation = receive_role_measurement::receive_rotation) {
                      auto before = std::move (record.before_application_handler);
                      record.before_application_handler = [rotation, before = std::move (before)] {
                          if (before)
                              before ();
                          receive_role_measurement::record_started (rotation);
                      };
                  }
                  node->application_work_enqueued ();
              },
              std::move (submit_owner),''')
p.write_text(s)
