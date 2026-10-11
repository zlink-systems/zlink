/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#include "runtime/client_server/raw_client_server_owner.hpp"
#include "runtime/mesh/raw_mesh_node_owner.hpp"
#include <gtest/gtest.h>
#include <atomic>
#include <zlink_enum.h>

namespace
{
std::atomic_size_t flow_calls{0};
}

extern "C" zlink_config_result_t
__real_zlink_socket_set_receive_flow_state (void *, zlink_receive_flow_state_t);
extern "C" zlink_config_result_t
__wrap_zlink_socket_set_receive_flow_state (void *socket, zlink_receive_flow_state_t state)
{
    ++flow_calls;
    return __real_zlink_socket_set_receive_flow_state (socket, state);
}

TEST (ClientReceivePressure, OnlyServerReceivesHostPressure)
{
    using namespace zlink::framework::runtime;
    application_job_queue_configuration_t configuration;
    configuration.effective_max_queued_application_jobs = 1;
    configuration.pause_threshold_percent = 100;
    auto jobs = std::make_shared<application_job_queue_t> (configuration);
    auto context = std::make_shared<zlink::context_t> ();
    auto failures = std::make_shared<runtime_failure_collector_t> ();
    client_server::raw_client_server_client_options_t options;
    options.client_routing_id = {'c'};
    options.admission.channel_name = "pressure";
    options.expected_server.advertised_endpoint = "inproc://client-receive-pressure";
    options.runtime_failures = failures;
    client_server::raw_client_server_client_t client (options, context);
    flow_calls = 0;
    client.start ();
    auto permit = jobs->try_reserve_supply ();
    ASSERT_TRUE (permit);
    EXPECT_EQ (zlink::framework::application_job_queue_pressure_state_t::paused,
               jobs->snapshot ().pressure_state);
    EXPECT_EQ (0u, flow_calls.load ());
    permit.reset ();
    EXPECT_EQ (0u, flow_calls.load ());
    client.close ();

    client_server::raw_client_server_server_options_t server_options;
    server_options.descriptor.channel_name = "pressure";
    server_options.descriptor.server_routing_id = {'s'};
    server_options.descriptor.advertised_endpoint = "inproc://client-receive-pressure";
    server_options.application_jobs = jobs;
    server_options.runtime_failures = failures;
    client_server::raw_client_server_server_t server (server_options, context);
    flow_calls = 0;
    server.start ();
    EXPECT_EQ (1u, flow_calls.load ());
    permit = jobs->try_reserve_supply ();
    ASSERT_TRUE (permit);
    EXPECT_EQ (2u, flow_calls.load ());
    permit.reset ();
    EXPECT_EQ (3u, flow_calls.load ());
    server.close ();
    mesh::raw_mesh_node_options_t mesh_options;
    mesh_options.descriptor.mesh_name = "pressure";
    mesh_options.descriptor.node_routing_id = {'n'};
    mesh_options.descriptor.lifecycle_generation = 1;
    mesh_options.descriptor.descriptor_revision = 1;
    mesh_options.descriptor.advertised_endpoint = "inproc://mesh-receive-pressure";
    mesh_options.application_jobs = jobs;
    mesh_options.runtime_failures = failures;
    mesh::raw_mesh_node_owner_t node (mesh_options, context);
    flow_calls = 0;
    node.start ();
    EXPECT_EQ (1u, flow_calls.load ());
    permit = jobs->try_reserve_supply ();
    ASSERT_TRUE (permit);
    EXPECT_EQ (2u, flow_calls.load ());
    permit.reset ();
    EXPECT_EQ (3u, flow_calls.load ());
    node.close ();
}
