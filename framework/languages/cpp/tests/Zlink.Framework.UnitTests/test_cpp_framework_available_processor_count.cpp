/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */

#include "runtime/utils/available_processor_count.hpp"
#include "runtime/dispatch/completion_turn.hpp"

#include <cassert>
#include <iostream>

int main ()
{
    using zlink::framework::runtime::available_processor_count;
    assert (available_processor_count () >= 1);
#if defined(__linux__)
    cpu_set_t original;
    CPU_ZERO (&original);
    assert (sched_getaffinity (0, sizeof (original), &original) == 0);
    const auto original_count = CPU_COUNT (&original);
    assert (available_processor_count () == static_cast<unsigned> (original_count));
    for (const unsigned requested : {1u, 2u, 8u}) {
        cpu_set_t limited;
        CPU_ZERO (&limited);
        unsigned selected = 0;
        for (int cpu = 0; cpu < CPU_SETSIZE && selected < requested; ++cpu) {
            if (CPU_ISSET (cpu, &original)) {
                CPU_SET (cpu, &limited);
                ++selected;
            }
        }
        assert (sched_setaffinity (0, sizeof (limited), &limited) == 0);
        assert (available_processor_count () == selected);
        zlink::framework::completion_owner_t owner;
        const auto workers = zlink::framework::detail::completion_dispatcher_worker_count ();
        assert (workers == std::max (2u, selected));
        std::cout << "affinity=" << selected << " completion_workers=" << workers << '\n';
        owner.close ();
        assert (sched_setaffinity (0, sizeof (original), &original) == 0);
    }
    assert (available_processor_count () == static_cast<unsigned> (original_count));
#endif
}
