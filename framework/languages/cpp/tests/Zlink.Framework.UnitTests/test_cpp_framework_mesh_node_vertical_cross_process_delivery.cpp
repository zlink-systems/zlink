/* SPDX-License-Identifier: Elastic-2.0 */
#include "../support/mesh_node_vertical_cases.hpp"
#include <zlink.hpp>
#include <atomic>
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/dispatch/completion_turn.hpp"
#include "runtime/dispatch/coroutine_executor.hpp"
#include <runtime/locations/location_repository.hpp>
#include "runtime/locations/in_memory_location_store.hpp"
#include "runtime/locations/live_location_reader.hpp"
#include "runtime/locations/location_runtime.hpp"
#include "runtime/locations/store_location_resolvers.hpp"
#include "runtime/dispatch/dispatch_limits.hpp"
#include "runtime/execution/infrastructure_wait_guard.hpp"
#include "runtime/mesh/mesh_node_host_service.hpp"
#include "runtime/mesh/mesh_metadata_codec.hpp"
#include "runtime/mesh/route_mesh_runtime_options_service.hpp"
#include "runtime/mesh/route_mesh_runtime_service.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "../support/loopback_tcp_endpoint.hpp"
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <latch>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>
#if defined(__unix__)
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
#include "../support/mesh_node_vertical_transport_fixture.hpp"

using namespace framework_mesh_node_vertical_fixture;

using namespace std::chrono_literals;

namespace
{
bool wait_until_admitted (zlink::framework::detail::mesh_node_runtime_t &node)
{
    // The transport drains its socket monitor from dispatch_ready, so a waiter
    // pumps the node the way the host service does instead of sleeping blind.
    const auto deadline = std::chrono::steady_clock::now () + 5s;
    while (std::chrono::steady_clock::now () < deadline) {
        if (node.admitted_peer_count () > 0)
            return true;
        (void) std::move (
          node.dispatch_ready ([] (const zlink::framework::runtime::host::ready_record_t &,
                                   const zlink::framework::runtime::host::receive_record_t &,
                                   std::vector<zlink::message_t>, auto) {}))
          .result ()
          .value ();
        std::this_thread::sleep_for (10ms);
    }
    std::fprintf (stderr, "[vertical] admission timeout rid=%s peers=%zu state=%d\n",
                  node.status ().routing_id ().to_string ().c_str (), node.admitted_peer_count (),
                  static_cast<int> (node.status ().state));
    return false;
}

bool wait_until_admitted_count (zlink::framework::detail::mesh_node_runtime_t &node,
                                std::size_t expected)
{
    const auto deadline = std::chrono::steady_clock::now () + 5s;
    while (std::chrono::steady_clock::now () < deadline) {
        if (node.admitted_peer_count () >= expected)
            return true;
        (void) std::move (
          node.dispatch_ready ([] (const zlink::framework::runtime::host::ready_record_t &,
                                   const zlink::framework::runtime::host::receive_record_t &,
                                   std::vector<zlink::message_t>, auto) {}))
          .result ()
          .value ();
        std::this_thread::sleep_for (10ms);
    }
    return false;
}

template <typename TSubmit> bool submit_until_ok (TSubmit submit)
{
    const auto deadline = std::chrono::steady_clock::now () + 5s;
    zlink::submit_result_t last = zlink::submit_result_t::internal_error;
    while (std::chrono::steady_clock::now () < deadline) {
        last = std::move (submit ()).result ().value ();
        if (last == zlink::submit_result_t::ok)
            return true;
        std::this_thread::sleep_for (10ms);
    }
    std::cerr << "last submit result=" << static_cast<int> (last) << '\n';
    return false;
}

void configure_vertical_route_fences (zlink::framework::detail::mesh_node_runtime_t &node)
{
    // This vertical test exercises the raw MeshNode transport without the
    // application Location Store. Supply the same non-zero owner fence that
    // the target objects receive from the minimal stateful runtime.
    node.configure_spot_route_fence_resolver (
      [&node] (zlink::routing_id_t target, std::string, std::uint64_t)
        -> zlink::framework::task_t<std::optional<zlink::framework::runtime::host::route_fence_t>> {
          const auto local = node.status ();
          if (target == local.routing_id ())
              co_return zlink::framework::runtime::host::route_fence_t{
                1, 1, local.lifecycle_generation ()};
          const auto peer = node.native_node ().transport ().topology ().peer (target.to_bytes ());
          if (!peer)
              co_return std::nullopt;
          co_return zlink::framework::runtime::host::route_fence_t{
            1, 1, peer->descriptor.lifecycle_generation};
      });
}

bool receive_one (zlink::framework::detail::mesh_node_runtime_t &node,
                  zlink::framework::runtime::host::record_kind_t expected_kind,
                  const std::string &expected_text,
                  const std::vector<std::uint8_t> &expected_metadata)
{
    const auto deadline = std::chrono::steady_clock::now () + 5s;
    while (std::chrono::steady_clock::now () < deadline) {
        bool matched = false;
        (void) std::move (
          node.dispatch_ready ([&] (const zlink::framework::runtime::host::ready_record_t &,
                                    const zlink::framework::runtime::host::receive_record_t &record,
                                    std::vector<zlink::message_t> parts, auto) {
              matched = matched
                        || (record.kind == expected_kind && !parts.empty ()
                            && parts.front ().to_string () == expected_text);
          }))
          .result ()
          .value ();
        if (matched)
            return true;
        std::this_thread::sleep_for (5ms);
    }
    return false;
}

bool reply_to_one_request (zlink::framework::detail::mesh_node_runtime_t &node,
                           zlink::framework::runtime::host::record_kind_t expected_kind,
                           const std::string &expected_text,
                           const std::string &reply_text)
{
    const auto deadline = std::chrono::steady_clock::now () + 5s;
    while (std::chrono::steady_clock::now () < deadline) {
        bool replied = false;
        (void) std::move (
          node.dispatch_ready ([&] (const zlink::framework::runtime::host::ready_record_t &,
                                    const zlink::framework::runtime::host::receive_record_t &record,
                                    std::vector<zlink::message_t> parts, auto) {
              if (record.kind == expected_kind && !parts.empty ()
                  && parts.front ().to_string () == expected_text) {
                  const std::vector<zlink::message_t> reply_parts{
                    zlink::message_t::from (reply_text)};
                  replied = zlink::framework::runtime::host::reply (record.reply_token, reply_parts)
                            == zlink::submit_result_t::ok;
              }
          }))
          .result ()
          .value ();
        if (replied)
            return true;
        std::this_thread::sleep_for (5ms);
    }
    return false;
}

bool receive_completion (zlink::framework::detail::mesh_node_runtime_t &node,
                         const zlink::framework::runtime::host::pending_operation_t &operation_id,
                         const std::string &expected_text)
{
    auto completion = node.await_completion (operation_id);
    const auto deadline = std::chrono::steady_clock::now () + 5s;
    while (!completion.await_ready () && std::chrono::steady_clock::now () < deadline) {
        // Only the binding receive owner progresses. No host dispatch turn
        // is needed between binding terminal and the registered result.
        (void) node.native_node ()
          .transport ()
          .pump_one (std::chrono::steady_clock::now (), false)
          .result ()
          .value ();
        std::this_thread::yield ();
    }
    if (!completion.await_ready () || !completion.result ())
        return false;
    const auto &settled = completion.result ().value ();
    return settled.record.operation_id == operation_id.id && settled.record.terminal_result == 0
           && !settled.parts.empty () && settled.parts.front ().to_string () == expected_text;
}
} // namespace

#if defined(__unix__)
namespace
{
// A child process reports each step's result to the parent as one status byte. A
// short write exits with the step's own code so the parent's exit-code check names it.
void write_status_byte (int fd_, char value_, int exit_code_)
{
    if (write (fd_, &value_, sizeof (value_)) != static_cast<ssize_t> (sizeof (value_)))
        _exit (exit_code_);
}

// A node process binds "tcp://127.0.0.1:*" and passes the endpoint it resolved to
// the other process through a pipe, so no port is reserved before the bind.
bool write_endpoint (int fd_, const std::string &endpoint_)
{
    const auto size = static_cast<std::uint32_t> (endpoint_.size ());
    return write (fd_, &size, sizeof (size)) == sizeof (size)
           && write (fd_, endpoint_.data (), endpoint_.size ())
                == static_cast<ssize_t> (endpoint_.size ());
}

std::optional<std::string> read_endpoint (int fd_)
{
    std::uint32_t size = 0;
    if (read (fd_, &size, sizeof (size)) != sizeof (size))
        return std::nullopt;
    std::string endpoint (size, '\0');
    if (read (fd_, endpoint.data (), endpoint.size ()) != static_cast<ssize_t> (endpoint.size ()))
        return std::nullopt;
    return endpoint;
}

} // namespace

namespace framework_mesh_node_vertical_tests
{
int run_cross_process_delivery ()
{
    const std::map<std::string, std::string> metadata_boundary{{"k", std::string (1018, 'v')}};
    const auto encoded_boundary =
      zlink::framework::detail::mesh_metadata_codec_t::encode (metadata_boundary);
    assert (encoded_boundary.size () == 1024);
    std::map<std::string, std::string> decoded_boundary;
    assert (
      zlink::framework::detail::mesh_metadata_codec_t::decode (encoded_boundary, decoded_boundary));
    assert (decoded_boundary == metadata_boundary);
    bool rejected_oversize = false;
    try {
        (void) zlink::framework::detail::mesh_metadata_codec_t::encode (
          {{"k", std::string (1019, 'v')}});
    }
    catch (const zlink::framework::framework_exception_t &) {
        rejected_oversize = true;
    }
    assert (rejected_oversize);
    std::map<std::string, std::string> malformed_output;
    assert (!zlink::framework::detail::mesh_metadata_codec_t::decode ({1, 1, 1, 0xc0, 0, 0},
                                                                      malformed_output));
    assert (!zlink::framework::detail::mesh_metadata_codec_t::decode (
      {1, 2, 1, 'k', 0, 1, 'a', 1, 'k', 0, 1, 'b'}, malformed_output));

    int endpoint_pipe[2];
    int direct_ack_pipe[2];
    int channel_ack_pipe[2];
    int request_ack_pipe[2];
    int completion_ack_pipe[2];
    int spot_request_ack_pipe[2];
    int reciprocal_ready_pipe[2];
    int reciprocal_child_ready_pipe[2];
    int reciprocal_child_stop_pipe[2];
    int reciprocal_child_endpoint_pipe[2];
    int reciprocal_endpoint_pipe[2];
    int formal_descriptor_pipe[2];
    int formal_ack_pipe[2];
    const auto read_byte_while_dispatching = [] (auto &node, int fd, char &value) {
        pollfd descriptor{fd, POLLIN, 0};
        while (true) {
            const int poll_result = poll (&descriptor, 1, 0);
            if (poll_result < 0) {
                if (errno == EINTR)
                    continue;
                return false;
            }
            if (poll_result > 0
                && (descriptor.revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) != 0) {
                const ssize_t bytes = read (fd, &value, sizeof (value));
                if (bytes >= 0)
                    return bytes == 1;
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                    continue;
                return false;
            }
            (void) std::move (
              node.dispatch_ready ([] (const zlink::framework::runtime::host::ready_record_t &,
                                       const zlink::framework::runtime::host::receive_record_t &,
                                       std::vector<zlink::message_t>, auto) {}))
              .result ()
              .value ();
            std::this_thread::sleep_for (10ms);
        }
    };
    if (pipe (endpoint_pipe) != 0 || pipe (direct_ack_pipe) != 0 || pipe (channel_ack_pipe) != 0
        || pipe (request_ack_pipe) != 0 || pipe (completion_ack_pipe) != 0
        || pipe (spot_request_ack_pipe) != 0 || pipe (reciprocal_ready_pipe) != 0
        || pipe (reciprocal_child_ready_pipe) != 0 || pipe (reciprocal_child_stop_pipe) != 0
        || pipe (formal_descriptor_pipe) != 0 || pipe (formal_ack_pipe) != 0
        || pipe (reciprocal_child_endpoint_pipe) != 0 || pipe (reciprocal_endpoint_pipe) != 0)
        return 1;

    const pid_t child = fork ();
    if (child < 0)
        return 1;
    if (child == 0) {
        zlink::framework::completion_owner_t completion_owner;
        close (endpoint_pipe[0]);
        close (direct_ack_pipe[0]);
        close (channel_ack_pipe[0]);
        close (request_ack_pipe[0]);
        close (completion_ack_pipe[1]);
        close (spot_request_ack_pipe[0]);
        close (reciprocal_ready_pipe[0]);
        close (reciprocal_child_ready_pipe[0]);
        close (reciprocal_child_ready_pipe[1]);
        close (reciprocal_child_stop_pipe[0]);
        close (reciprocal_child_stop_pipe[1]);
        close (reciprocal_endpoint_pipe[1]);
        close (formal_descriptor_pipe[0]);
        close (formal_ack_pipe[0]);
        close (reciprocal_child_endpoint_pipe[0]);
        close (reciprocal_child_endpoint_pipe[1]);
        auto state = make_node ("tcp://127.0.0.1:*", "vertical-b");
        zlink::framework::detail::mesh_node_runtime_t node (state);
        node.start ().result ().value ();
        auto target_spot = node.get_or_create_spot_async ("target-spot").result ().value ();
        auto target_actor = node.create_actor ("vertical.actor", "target-actor", {}, 5s);
        // The Actor send names the target node generation its authority resolved.
        const std::uint64_t formal_descriptors[3]{target_spot.status ().lifecycle_generation (),
                                                  target_actor.ref ().object_generation (),
                                                  node.status ().lifecycle_generation ()};
        if (write (formal_descriptor_pipe[1], formal_descriptors, sizeof (formal_descriptors))
            != sizeof (formal_descriptors)) {
            _exit (8);
        }
        close (formal_descriptor_pipe[1]);
        if (!write_endpoint (endpoint_pipe[1], node.status ().local_endpoint ()))
            _exit (2);
        close (endpoint_pipe[1]);
        const auto reciprocal_endpoint = read_endpoint (reciprocal_endpoint_pipe[0]);
        close (reciprocal_endpoint_pipe[0]);
        if (!reciprocal_endpoint)
            _exit (14);
        node.connect_peer (zlink::routing_id_t::from (std::string ("vertical-c")),
                           *reciprocal_endpoint);

        const bool multi_peer_admitted = wait_until_admitted_count (node, 2);
        const char reciprocal_ready = multi_peer_admitted ? 1 : 0;
        write_status_byte (reciprocal_ready_pipe[1], reciprocal_ready, 15);
        close (reciprocal_ready_pipe[1]);

        const std::map<std::string, std::string> metadata{{"trace-id", "vertical-1"},
                                                          {"tenant", "sample"}};
        const auto encoded_metadata =
          zlink::framework::detail::mesh_metadata_codec_t::encode (metadata);
        const bool admitted = wait_until_admitted (node);
        const bool direct =
          receive_one (node, zlink::framework::runtime::host::record_kind_t::node_send, "direct",
                       encoded_metadata);
        const char direct_ack = direct ? 1 : 0;
        write_status_byte (direct_ack_pipe[1], direct_ack, 16);
        close (direct_ack_pipe[1]);
        const bool channel =
          receive_one (node, zlink::framework::runtime::host::record_kind_t::channel_send,
                       "channel", encoded_metadata);
        const char channel_ack = channel ? 1 : 0;
        write_status_byte (channel_ack_pipe[1], channel_ack, 17);
        close (channel_ack_pipe[1]);
        const bool request = reply_to_one_request (
          node, zlink::framework::runtime::host::record_kind_t::node_request, "request", "reply");
        const char request_ack = request ? 1 : 0;
        write_status_byte (request_ack_pipe[1], request_ack, 18);
        close (request_ack_pipe[1]);
        char completion_ack = 0;
        if (request
            && read (completion_ack_pipe[0], &completion_ack, sizeof (completion_ack))
                 != static_cast<ssize_t> (sizeof (completion_ack)))
            _exit (19);
        close (completion_ack_pipe[0]);
        const bool spot =
          receive_one (node, zlink::framework::runtime::host::record_kind_t::spot_send, "spot",
                       encoded_metadata);
        const char spot_ack = spot ? 1 : 0;
        write_status_byte (formal_ack_pipe[1], spot_ack, 20);
        const bool spot_request =
          reply_to_one_request (node, zlink::framework::runtime::host::record_kind_t::spot_request,
                                "spot-request", "spot-reply");
        const char spot_request_ack = spot_request ? 1 : 0;
        write_status_byte (spot_request_ack_pipe[1], spot_request_ack, 21);
        close (spot_request_ack_pipe[1]);
        const bool actor =
          receive_one (node, zlink::framework::runtime::host::record_kind_t::actor_send, "actor",
                       encoded_metadata);
        const char actor_ack = actor ? 1 : 0;
        write_status_byte (formal_ack_pipe[1], actor_ack, 22);
        close (formal_ack_pipe[1]);
        node.stop ();
        int exit_code = 0;
        if (!admitted)
            exit_code = 3;
        else if (!multi_peer_admitted)
            exit_code = 12;
        else if (!direct)
            exit_code = 4;
        else if (!channel)
            exit_code = 5;
        else if (!request)
            exit_code = 6;
        else if (completion_ack != 1)
            exit_code = 7;
        else if (!spot)
            exit_code = 9;
        else if (!spot_request)
            exit_code = 11;
        else if (!actor)
            exit_code = 10;
        completion_owner.close ();
        _exit (exit_code);
    }

    close (endpoint_pipe[1]);
    close (direct_ack_pipe[1]);
    close (channel_ack_pipe[1]);
    close (request_ack_pipe[1]);
    close (completion_ack_pipe[0]);
    close (spot_request_ack_pipe[1]);
    close (reciprocal_ready_pipe[1]);
    close (formal_descriptor_pipe[1]);
    close (formal_ack_pipe[1]);
    close (reciprocal_endpoint_pipe[0]);
    const auto read_node_endpoint = read_endpoint (endpoint_pipe[0]);
    close (endpoint_pipe[0]);
    if (!read_node_endpoint)
        return 1;
    const std::string endpoint = *read_node_endpoint;
    std::uint64_t formal_descriptors[3]{};
    assert (read (formal_descriptor_pipe[0], formal_descriptors, sizeof (formal_descriptors))
            == sizeof (formal_descriptors));
    close (formal_descriptor_pipe[0]);

    const pid_t reciprocal_child = fork ();
    assert (reciprocal_child >= 0);
    if (reciprocal_child == 0) {
        zlink::framework::completion_owner_t completion_owner;
        close (reciprocal_child_ready_pipe[0]);
        close (reciprocal_child_stop_pipe[1]);
        close (reciprocal_child_endpoint_pipe[0]);
        close (reciprocal_endpoint_pipe[1]);
        auto reciprocal_state = make_node ("tcp://127.0.0.1:*", "vertical-c");
        reciprocal_state->peer_connections.push_back (zlink::framework::mesh_peer_connection_t{
          1, zlink::routing_id_t::from (std::string ("vertical-b")), endpoint});
        zlink::framework::detail::mesh_node_runtime_t reciprocal_node (reciprocal_state);
        reciprocal_node.start ().result ().value ();
        const bool endpoint_sent = write_endpoint (reciprocal_child_endpoint_pipe[1],
                                                   reciprocal_node.status ().local_endpoint ());
        close (reciprocal_child_endpoint_pipe[1]);
        const char ready = endpoint_sent && wait_until_admitted (reciprocal_node) ? 1 : 0;
        write_status_byte (reciprocal_child_ready_pipe[1], ready, 14);
        close (reciprocal_child_ready_pipe[1]);
        char stop = 0;
        (void) read_byte_while_dispatching (reciprocal_node, reciprocal_child_stop_pipe[0], stop);
        close (reciprocal_child_stop_pipe[0]);
        reciprocal_node.stop ();
        completion_owner.close ();
        _exit (ready == 1 && stop == 1 ? 0 : 13);
    }
    zlink::framework::completion_owner_t completion_owner;
    close (reciprocal_child_ready_pipe[1]);
    close (reciprocal_child_stop_pipe[0]);
    close (reciprocal_child_endpoint_pipe[1]);
    const auto reciprocal_endpoint = read_endpoint (reciprocal_child_endpoint_pipe[0]);
    close (reciprocal_child_endpoint_pipe[0]);
    assert (reciprocal_endpoint);
    assert (write_endpoint (reciprocal_endpoint_pipe[1], *reciprocal_endpoint));
    close (reciprocal_endpoint_pipe[1]);
    char reciprocal_child_ready = 0;
    assert (read (reciprocal_child_ready_pipe[0], &reciprocal_child_ready,
                  sizeof (reciprocal_child_ready))
            == sizeof (reciprocal_child_ready));
    close (reciprocal_child_ready_pipe[0]);
    assert (reciprocal_child_ready == 1);

    auto state = make_node ("tcp://127.0.0.1:*", "vertical-a");
    state->peer_connections.push_back (zlink::framework::mesh_peer_connection_t{1, {}, endpoint});
    zlink::framework::detail::mesh_node_runtime_t node (state);
    configure_vertical_route_fences (node);
    node.start ().result ().value ();
    assert (wait_until_admitted (node));
    char reciprocal_ready = 0;
    assert (read_byte_while_dispatching (node, reciprocal_ready_pipe[0], reciprocal_ready));
    close (reciprocal_ready_pipe[0]);
    assert (reciprocal_ready == 1);

    const std::map<std::string, std::string> metadata{{"trace-id", "vertical-1"},
                                                      {"tenant", "sample"}};
    const std::vector<zlink::message_t> direct_parts{
      zlink::message_t::from (std::string ("direct"))};
    assert (submit_until_ok ([&] {
        return node.send_to_node (zlink::routing_id_t::from (std::string ("vertical-b")),
                                  direct_parts, metadata);
    }));
    char direct_ack = 0;
    assert (read (direct_ack_pipe[0], &direct_ack, sizeof (direct_ack)) == sizeof (direct_ack));
    close (direct_ack_pipe[0]);
    assert (direct_ack == 1);
    const std::vector<zlink::message_t> channel_parts{
      zlink::message_t::from (std::string ("channel"))};
    assert (
      submit_until_ok ([&] { return node.send_to_channel ("work", channel_parts, metadata); }));
    char channel_ack = 0;
    assert (read (channel_ack_pipe[0], &channel_ack, sizeof (channel_ack)) == sizeof (channel_ack));
    close (channel_ack_pipe[0]);
    assert (channel_ack == 1);
    const std::vector<zlink::message_t> request_parts{
      zlink::message_t::from (std::string ("request"))};
    zlink::framework::runtime::host::pending_operation_t operation_id;
    assert (std::move (node.request_to_node (zlink::routing_id_t::from (std::string ("vertical-b")),
                                             request_parts, operation_id, 5s, metadata))
              .result ()
              .value ()
            == zlink::submit_result_t::ok);
    char request_ack = 0;
    assert (read (request_ack_pipe[0], &request_ack, sizeof (request_ack)) == sizeof (request_ack));
    close (request_ack_pipe[0]);
    assert (request_ack == 1);
    assert (receive_completion (node, operation_id, "reply"));
    const char completion_ack = 1;
    assert (write (completion_ack_pipe[1], &completion_ack, sizeof (completion_ack))
            == sizeof (completion_ack));
    close (completion_ack_pipe[1]);
    const std::vector<zlink::message_t> spot_parts{zlink::message_t::from (std::string ("spot"))};
    assert (submit_until_ok ([&] {
        return node.send_to_spot (
          "source-spot", zlink::routing_id_t::from (std::string ("vertical-b")), "target-spot",
          formal_descriptors[0], spot_parts,
          zlink::framework::detail::mesh_metadata_codec_t::encode (metadata));
    }));
    char spot_ack = 0;
    assert (read (formal_ack_pipe[0], &spot_ack, sizeof (spot_ack)) == sizeof (spot_ack));
    assert (spot_ack == 1);
    const std::vector<zlink::message_t> spot_request_parts{
      zlink::message_t::from (std::string ("spot-request"))};
    zlink::framework::runtime::host::pending_operation_t spot_operation_id;
    assert (
      std::move (node.request_to_spot (
                   "source-spot", zlink::routing_id_t::from (std::string ("vertical-b")),
                   "target-spot", formal_descriptors[0], spot_request_parts, spot_operation_id, 5s,
                   zlink::framework::detail::mesh_metadata_codec_t::encode (metadata)))
        .result ()
        .value ()
      == zlink::submit_result_t::ok);
    char spot_request_ack = 0;
    assert (read (spot_request_ack_pipe[0], &spot_request_ack, sizeof (spot_request_ack))
            == sizeof (spot_request_ack));
    close (spot_request_ack_pipe[0]);
    assert (spot_request_ack == 1);
    assert (receive_completion (node, spot_operation_id, "spot-reply"));
    const std::vector<zlink::message_t> actor_parts{zlink::message_t::from (std::string ("actor"))};
    assert (submit_until_ok ([&] {
        return node.send_to_actor (
          zlink::framework::runtime::host::public_host_runtime_t::remote_actor_ref (
            zlink::routing_id_t::from (std::string ("vertical-b")), "target-actor",
            formal_descriptors[1]),
          actor_parts, zlink::framework::detail::mesh_metadata_codec_t::encode (metadata), 1, 1,
          std::nullopt, formal_descriptors[2]);
    }));
    char actor_ack = 0;
    assert (read (formal_ack_pipe[0], &actor_ack, sizeof (actor_ack)) == sizeof (actor_ack));
    close (formal_ack_pipe[0]);
    assert (actor_ack == 1);

    auto local_source = node.get_or_create_spot_async ("source-spot").result ().value ();
    auto local_target = node.get_or_create_spot_async ("local-target").result ().value ();
    const std::vector<zlink::message_t> local_request_parts{
      zlink::message_t::from (std::string ("local-request"))};
    const auto local_node_rid = node.status ().routing_id ();
    const auto local_target_generation = local_target.status ().lifecycle_generation ();
    const std::vector<zlink::message_t> local_send_parts{
      zlink::message_t::from (std::string ("local-send"))};
    assert (std::move (local_source.send_to_spot (local_node_rid, "local-target",
                                                  local_target_generation, local_send_parts))
              .result ()
              .value ()
            == zlink::submit_result_t::ok);
    bool local_send_delivered = false;
    (void) std::move (
      node.dispatch_ready ([&] (const zlink::framework::runtime::host::ready_record_t &owner,
                                const zlink::framework::runtime::host::receive_record_t &record,
                                std::vector<zlink::message_t> parts, auto) {
          local_send_delivered =
            owner.owner_kind == zlink::framework::runtime::host::owner_kind_t::spot
            && owner.spot_id == "local-target"
            && record.kind == zlink::framework::runtime::host::record_kind_t::spot_send
            && !parts.empty () && parts.front ().to_string () == "local-send";
      }))
      .result ()
      .value ();
    assert (local_send_delivered);

    zlink::framework::runtime::host::pending_operation_t local_timeout_operation;
    int local_timeout_callbacks = 0;
    std::promise<void> local_timeout_delivered;
    auto local_timeout_ready = local_timeout_delivered.get_future ();
    assert (std::move (
              local_source.request_to_spot (
                local_node_rid, "local-target", local_target_generation, local_request_parts,
                local_timeout_operation, zlink::send_flags_t::none, 25ms, {},
                [&] (zlink::framework::runtime::foundation::operation_terminal_t terminal,
                     zlink::framework::result_t<std::vector<zlink::message_t>>) {
                    if (terminal
                        == zlink::framework::runtime::foundation::operation_terminal_t::timed_out)
                        ++local_timeout_callbacks;
                    local_timeout_delivered.set_value ();
                }))
              .result ()
              .value ()
            == zlink::submit_result_t::ok);
    std::this_thread::sleep_for (40ms);
    (void) std::move (
      node.dispatch_ready ([] (const zlink::framework::runtime::host::ready_record_t &,
                               const zlink::framework::runtime::host::receive_record_t &,
                               std::vector<zlink::message_t>, auto) {}))
      .result ()
      .value ();
    // Expiry claims the entry in this ingress turn; the already-reserved
    // dispatcher delivers the callback in a new turn.
    assert (local_timeout_ready.wait_for (1s) == std::future_status::ready);
    assert (local_timeout_callbacks == 1);

    zlink::framework::runtime::host::pending_operation_t local_shutdown_operation;
    int local_shutdown_callbacks = 0;
    assert (
      std::move (local_source.request_to_spot (
                   local_node_rid, "local-target", local_target_generation, local_request_parts,
                   local_shutdown_operation, zlink::send_flags_t::none, 5s, {},
                   [&] (zlink::framework::runtime::foundation::operation_terminal_t terminal,
                        zlink::framework::result_t<std::vector<zlink::message_t>>) {
                       if (terminal
                           == zlink::framework::runtime::foundation::operation_terminal_t::shutdown)
                           ++local_shutdown_callbacks;
                   }))
        .result ()
        .value ()
      == zlink::submit_result_t::ok);

    zlink::framework::runtime::host::pending_operation_t callbackless_shutdown_operation;
    assert (std::move (local_source.request_to_spot (local_node_rid, "local-target",
                                                     local_target_generation, local_request_parts,
                                                     callbackless_shutdown_operation,
                                                     zlink::send_flags_t::none, 5s, {}))
              .result ()
              .value ()
            == zlink::submit_result_t::ok);
    auto callbackless_wait = std::async (std::launch::async, [&] {
        return node.wait_for_completion (callbackless_shutdown_operation, 5s);
    });

    auto race_state = make_node ("tcp://127.0.0.1:*", "local-race");
    zlink::framework::detail::mesh_node_runtime_t race_node (race_state);
    configure_vertical_route_fences (race_node);
    race_node.start ().result ().value ();
    auto race_source = race_node.get_or_create_spot_async ("race-source").result ().value ();
    auto race_target = race_node.get_or_create_spot_async ("race-target").result ().value ();
    const auto race_node_rid = race_node.status ().routing_id ();
    const auto race_target_generation = race_target.status ().lifecycle_generation ();
    const std::vector<zlink::message_t> race_parts{zlink::message_t::from (std::string ("race"))};
    std::atomic_bool submit_race{true};
    std::atomic_int accepted_race_requests{0};
    std::atomic_int terminal_race_callbacks{0};
    std::thread race_submitter ([&] {
        while (submit_race.load (std::memory_order_acquire)) {
            zlink::framework::runtime::host::pending_operation_t operation;
            const auto submitted =
              std::move (race_source.request_to_spot (
                           race_node_rid, "race-target", race_target_generation, race_parts,
                           operation, zlink::send_flags_t::none, 1s, {},
                           [&] (zlink::framework::runtime::foundation::operation_terminal_t,
                                zlink::framework::result_t<std::vector<zlink::message_t>>) {
                               terminal_race_callbacks.fetch_add (1, std::memory_order_relaxed);
                           }))
                .result ()
                .value ();
            if (submitted == zlink::submit_result_t::ok) {
                accepted_race_requests.fetch_add (1, std::memory_order_relaxed);
            } else {
                std::this_thread::sleep_for (1ms);
            }
        }
    });
    std::this_thread::sleep_for (20ms);
    race_node.stop ();
    submit_race.store (false, std::memory_order_release);
    race_submitter.join ();
    assert (accepted_race_requests.load (std::memory_order_relaxed)
            == terminal_race_callbacks.load (std::memory_order_relaxed));

    int status = 0;
    waitpid (child, &status, 0);
    node.stop ();
    const auto callbackless_result = callbackless_wait.get ();
    assert (!callbackless_result);
    assert (callbackless_result.error () != nullptr);
    assert (zlink::framework::detail::boundary_state (*callbackless_result.error ())
            == zlink::framework::detail::boundary_error_t::shutdown);
    assert (local_shutdown_callbacks == 1);
    const char reciprocal_stop = 1;
    assert (write (reciprocal_child_stop_pipe[1], &reciprocal_stop, sizeof (reciprocal_stop))
            == sizeof (reciprocal_stop));
    close (reciprocal_child_stop_pipe[1]);
    int reciprocal_status = 0;
    waitpid (reciprocal_child, &reciprocal_status, 0);
    assert (WIFEXITED (reciprocal_status));
    assert (WEXITSTATUS (reciprocal_status) == 0);
    return WIFEXITED (status) ? WEXITSTATUS (status) : 4;
}
} // namespace framework_mesh_node_vertical_tests

#endif
