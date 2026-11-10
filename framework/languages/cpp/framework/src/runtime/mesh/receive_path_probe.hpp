/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once

#if defined(ZLINK_FRAMEWORK_TEST_CREATION_PROBE)
#include <cstddef>

namespace zlink::framework::runtime::mesh
{
struct service_mailbox_record_t;

// Installed only by the receive ownership fixture, on its ingress thread.
struct receive_path_probe_t
{
    std::size_t classifications = 0;
    std::size_t port_leases = 0;
    void *context = nullptr;
    void (*received) (void *, service_mailbox_record_t &) = nullptr;
};
inline thread_local receive_path_probe_t *receive_path_probe = nullptr;
}
#endif
