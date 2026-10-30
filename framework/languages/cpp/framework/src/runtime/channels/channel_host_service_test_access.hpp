/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once

#include "runtime/channels/channel_host_service.hpp"
#include "runtime/dispatch/offload_executor.hpp"

namespace zlink::framework::runtime
{
// Runtime-private fixture adapter. The executor is installed before receive starts.
struct manual_channel_server_test_access_t
{
    manual_channel_server_test_access_t (message_bus_t bus,
                                         channel_snapshot_t channel,
                                         service_provider_t &services,
                                         serializer_registry_t &serializers,
                                         const handler_registry_t &handlers,
                                         std::atomic_bool &stop,
                                         std::shared_ptr<zlink::context_t> context,
                                         std::shared_ptr<application_job_queue_t> jobs,
                                         std::shared_ptr<listener_status_registry_t> listeners,
                                         std::unique_ptr<offload_executor_t> executor);
    ~manual_channel_server_test_access_t ();
    void run ();
    void signal_activity ();
    std::size_t pending_messages () const;
    std::size_t peer_count () const;

  private:
    std::unique_ptr<channel_host_service_t::server_loop_t> _loop;
};
} // namespace zlink::framework::runtime
