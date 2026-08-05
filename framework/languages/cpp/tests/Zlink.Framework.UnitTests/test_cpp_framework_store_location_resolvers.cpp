/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/locations/in_memory_location_store.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include <runtime/locations/location_repository.hpp>
#include "runtime/locations/location_auto_connect_host_service.hpp"
#include "runtime/locations/location_runtime.hpp"
#include "runtime/locations/store_location_resolvers.hpp"
#include "runtime/channels/channel_runtime_manager.hpp"
#include "runtime/channels/channel_runtime.hpp"
#include "runtime/client_server/client_server_location_runtime.hpp"
#include "runtime/mesh/user_spot_terminal_mapping.hpp"
#include "runtime/streams/stream_runtime.hpp"

#include <gtest/gtest.h>
#include <zlink/framework.hpp>

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace
{

using zlink::framework::actor_location_t;
using zlink::framework::location_kind_t;
using zlink::framework::location_options_t;
using zlink::framework::location_owner_token_t;
using zlink::framework::location_page_t;
using zlink::framework::location_page_request_t;
using zlink::framework::location_runtime_query_t;
using zlink::framework::location_runtime_status_t;
using zlink::framework::location_role_t;
using zlink::framework::location_service_summary_filter_t;
using zlink::framework::location_service_summary_t;
using zlink::framework::location_topology_filter_t;
using zlink::framework::location_topology_entry_t;
using zlink::framework::location_topology_state_t;
using zlink::framework::location_write_intent_t;
using zlink::framework::location_write_status_t;
using zlink::framework::route_kind_t;
using zlink::framework::route_location_key_t;
using zlink::framework::spot_location_t;
using zlink::framework::runtime::in_memory_location_repository_t;
using zlink::framework::runtime::in_memory_location_store_t;
using zlink::framework::runtime::location_auto_connect_host_service_t;
using zlink::framework::runtime::location_runtime_t;
using zlink::framework::runtime::store_location_runtime_query_t;
using zlink::framework::runtime::store_location_resolvers_t;

location_owner_token_t live_owner_token (
  zlink::framework::location_repository_t &store,
  const std::string &owner_id)
{
    const auto lease =
      store.read_owner_lease (owner_id).result ().value ();
    const auto *found =
      std::get_if<zlink::framework::owner_lease_found_t> (
        &lease);
    if (found == nullptr)
        throw std::runtime_error (
          "test owner lease is not active");
    return found->token;
}

std::vector<std::byte> user_spot_authority_payload (
  std::string_view spot_id,
  std::uint64_t generation)
{
    const auto value = "zlink:user-spot:ready:v1\nplay\n"
                       + std::string (spot_id) + "\n"
                       + std::to_string (generation) + "\n1";
    std::vector<std::byte> payload;
    payload.reserve (value.size ());
    for (const auto character : value)
        payload.push_back (static_cast<std::byte> (
          static_cast<unsigned char> (character)));
    return payload;
}

void seed_mesh_node (
  in_memory_location_repository_t &store,
  std::string owner_id,
  std::string mesh_name,
  std::string rid,
  std::string endpoint,
  zlink::framework::framework_runtime_state_t state =
    zlink::framework::framework_runtime_state_t::serving)
{
    const auto claim = store.claim_owner_lease (
      owner_id, std::chrono::seconds (30)).result ().value ();
    const auto *claimed =
      std::get_if<zlink::framework::owner_lease_claimed_t> (&claim);
    if (claimed == nullptr)
        throw std::runtime_error ("mesh descriptor owner seed failed");
    const auto owner = claimed->token;
    const auto result = store.update_mesh_node (
      zlink::framework::mesh_node_descriptor_t{
        .mesh_name = std::move (mesh_name),
        .rid = zlink::routing_id_t::from (rid),
        .lifecycle_generation = 1,
        .descriptor_revision = 1,
        .endpoint = std::move (endpoint),
        .application_version = 1,
        .object_role = zlink::framework::object_role_t::server,
        .state = state,
        .security_identity = "test",
        .owner_id = owner.owner_id,
        .lease_generation = owner.lease_generation},
      location_write_intent_t::new_claim).result ().value ();
    if (result.status != location_write_status_t::stored)
        throw std::runtime_error ("mesh descriptor seed failed");
}

class test_location_repository_t : public zlink::framework::location_repository_t
{
  public:
    void set_authority (std::string key, zlink::framework::authority_snapshot_t snapshot)
    {
        authorities.insert_or_assign (std::move (key), std::move (snapshot));
    }

    zlink::framework::task_t<
      zlink::framework::location_write_result_t>
    update_mesh_node (
      zlink::framework::mesh_node_descriptor_t descriptor,
      zlink::framework::location_write_intent_t intent) override
    {
        return _inner.update_mesh_node (
          std::move (descriptor), intent);
    }

    zlink::framework::task_t<
      zlink::framework::location_write_status_t>
    remove_mesh_node (
      zlink::framework::mesh_node_descriptor_key_t key,
      zlink::framework::location_owner_token_t owner) override
    {
        return _inner.remove_mesh_node (
          std::move (key), std::move (owner));
    }

    zlink::framework::task_t<
      zlink::framework::location_page_t<
        zlink::framework::mesh_node_descriptor_t>>
    list_mesh_nodes (
      std::string mesh_name,
      zlink::framework::location_page_request_t page = {}) override
    {
        return _inner.list_mesh_nodes (
          std::move (mesh_name), std::move (page));
    }

    zlink::framework::task_t<zlink::framework::location_write_result_t>
    update_client_server (
      zlink::framework::client_server_server_descriptor_t descriptor,
      zlink::framework::location_write_intent_t intent) override
    {
        return _inner.update_client_server (std::move (descriptor), intent);
    }

    zlink::framework::task_t<zlink::framework::location_write_status_t>
    remove_client_server (
      zlink::framework::client_server_server_descriptor_key_t key,
      zlink::framework::location_owner_token_t owner) override
    {
        return _inner.remove_client_server (std::move (key), std::move (owner));
    }

    zlink::framework::task_t<zlink::framework::location_page_t<
      zlink::framework::client_server_server_descriptor_t>>
    list_client_servers (std::string channel_name,
                         zlink::framework::location_page_request_t page = {}) override
    {
        return _inner.list_client_servers (std::move (channel_name), std::move (page));
    }

    zlink::framework::task_t<zlink::framework::location_write_result_t>
    update_fanout_publisher (
      zlink::framework::fanout_publisher_descriptor_t descriptor,
      zlink::framework::location_write_intent_t intent) override
    {
        return _inner.update_fanout_publisher (std::move (descriptor), intent);
    }

    zlink::framework::task_t<zlink::framework::location_write_status_t>
    remove_fanout_publisher (
      zlink::framework::fanout_publisher_descriptor_key_t key,
      zlink::framework::location_owner_token_t owner) override
    {
        return _inner.remove_fanout_publisher (std::move (key), std::move (owner));
    }

    zlink::framework::task_t<zlink::framework::location_page_t<
      zlink::framework::fanout_publisher_descriptor_t>>
    list_fanout_publishers (std::string channel_name,
                            zlink::framework::location_page_request_t page = {}) override
    {
        return _inner.list_fanout_publishers (std::move (channel_name), std::move (page));
    }

    zlink::framework::task_t<
      zlink::framework::owner_lease_claim_result_t>
    claim_owner_lease (
      std::string owner_id,
      std::chrono::milliseconds lease_ttl) override
    {
        return _inner.claim_owner_lease (
          std::move (owner_id), lease_ttl);
    }

    zlink::framework::task_t<
      zlink::framework::owner_lease_read_result_t>
    read_owner_lease (std::string owner_id) override
    {
        return _inner.read_owner_lease (std::move (owner_id));
    }

    zlink::framework::task_t<
      zlink::framework::owner_lease_renew_result_t>
    renew_owner_lease (
      zlink::framework::location_owner_token_t token,
      std::chrono::milliseconds lease_ttl) override
    {
        return _inner.renew_owner_lease (
          std::move (token), lease_ttl);
    }

    zlink::framework::task_t<
      zlink::framework::owner_lease_release_result_t>
    release_owner_lease (
      zlink::framework::location_owner_token_t token) override
    {
        return _inner.release_owner_lease (std::move (token));
    }

    zlink::framework::task_t<std::int64_t> remove_all_by_owner (
      zlink::framework::location_owner_token_t owner) override
    {
        return _inner.remove_all_by_owner (std::move (owner));
    }

    zlink::framework::task_t<zlink::framework::authority_read_result_t>
    read_authority (zlink::framework::authority_key_t key,
                    std::stop_token cancellation = {}) override
    {
        if (key.value.starts_with ("zla1:a:"))
            resolve_actor_count.fetch_add (1, std::memory_order_relaxed);
        if (const auto found = authorities.find (key.value); found != authorities.end ()) {
            auto snapshot = found->second;
            snapshot.store_now = std::chrono::system_clock::now ();
            return zlink::framework::task_t<zlink::framework::authority_read_result_t> (
              zlink::framework::result_t<zlink::framework::authority_read_result_t>::success (
                zlink::framework::authority_read_result_t{std::move (snapshot)}));
        }
        return _inner.read_authority (std::move (key), cancellation);
    }

    zlink::framework::task_t<zlink::framework::authority_compare_exchange_result_t>
    compare_exchange_authority (
      zlink::framework::authority_key_t key,
      std::string expected_store_version,
      zlink::framework::authority_mutation_t mutation,
      std::stop_token cancellation = {}) override
    {
        return _inner.compare_exchange_authority (
          std::move (key), std::move (expected_store_version),
          std::move (mutation),
          cancellation);
    }

    zlink::framework::task_t<zlink::framework::authority_scan_result_t>
    list_authorities (
      std::string prefix,
      std::optional<zlink::framework::authority_scan_cursor_t> cursor,
      std::size_t limit,
      std::stop_token cancellation = {}) override
    {
        return _inner.list_authorities (
          std::move (prefix), std::move (cursor), limit, cancellation);
    }

    zlink::framework::task_t<std::optional<
      zlink::framework::creation_terminal_record_t>>
    read_creation_terminal (
      zlink::framework::creation_operation_identity_t operation,
      std::stop_token cancellation = {}) override
    {
        return _inner.read_creation_terminal (std::move (operation), cancellation);
    }

    zlink::framework::task_t<zlink::framework::object_reserve_result_t>
    reserve (zlink::framework::object_reserve_request_t request,
             std::stop_token cancellation = {}) override
    {
        if (force_reserve_conflict.exchange (
              false, std::memory_order_acq_rel)) {
            return zlink::framework::task_t<
              zlink::framework::object_reserve_result_t> (
              zlink::framework::result_t<
                zlink::framework::object_reserve_result_t>::
                success (
                  zlink::framework::object_reserve_conflict_t{
                    zlink::framework::authority_snapshot_t{}}));
        }
        return _inner.reserve (std::move (request), cancellation);
    }

    zlink::framework::task_t<
      zlink::framework::object_complete_creation_result_t>
    complete_creation (
      zlink::framework::object_complete_creation_request_t request,
      std::stop_token cancellation = {}) override
    {
        return _inner.complete_creation (std::move (request), cancellation);
    }

    zlink::framework::task_t<zlink::framework::object_commit_result_t>
    commit (zlink::framework::object_commit_request_t request,
            std::stop_token cancellation = {}) override
    {
        return _inner.commit (std::move (request), cancellation);
    }

    zlink::framework::task_t<zlink::framework::object_abort_result_t>
    abort (zlink::framework::object_abort_request_t request,
           std::stop_token cancellation = {}) override
    {
        abort_count.fetch_add (1, std::memory_order_relaxed);
        return _inner.abort (std::move (request), cancellation);
    }

    zlink::framework::task_t<
      zlink::framework::relocation_capacity_reserve_result_t>
    reserve_relocation_capacity (
      zlink::framework::relocation_capacity_reserve_request_t request,
      std::stop_token cancellation = {}) override
    {
        return _inner.reserve_relocation_capacity (
          std::move (request), cancellation);
    }

    zlink::framework::task_t<
      zlink::framework::relocation_capacity_abort_result_t>
    abort_relocation_capacity (
      zlink::framework::relocation_capacity_fence_t fence,
      std::stop_token cancellation = {}) override
    {
        return _inner.abort_relocation_capacity (
          std::move (fence), cancellation);
    }

    zlink::framework::task_t<zlink::framework::aggregate_prepare_result_t>
    prepare_aggregate (
      zlink::framework::aggregate_prepare_request_t request,
      std::stop_token cancellation = {}) override
    {
        return _inner.prepare_aggregate (std::move (request), cancellation);
    }

    zlink::framework::task_t<zlink::framework::aggregate_commit_result_t>
    commit_aggregate (zlink::framework::aggregate_fence_t fence,
                      std::stop_token cancellation = {}) override
    {
        return _inner.commit_aggregate (std::move (fence), cancellation);
    }

    zlink::framework::task_t<zlink::framework::aggregate_abort_result_t>
    abort_aggregate (zlink::framework::aggregate_fence_t fence,
                     std::stop_token cancellation = {}) override
    {
        return _inner.abort_aggregate (std::move (fence), cancellation);
    }

    std::atomic_size_t abort_count{0};
    std::atomic_size_t resolve_spot_count{0};
    std::atomic_size_t resolve_actor_count{0};
    std::atomic_bool force_reserve_conflict{false};

  private:
    std::map<std::string, zlink::framework::authority_snapshot_t> authorities;
    in_memory_location_repository_t _inner;
};

class other_test_location_repository_t : public test_location_repository_t
{
};

class failing_owner_lease_store_t final : public test_location_repository_t
{
  public:
    zlink::framework::task_t<
      zlink::framework::owner_lease_renew_result_t>
    renew_owner_lease (
      zlink::framework::location_owner_token_t,
      std::chrono::milliseconds) override
    {
        return zlink::framework::task_t<
          zlink::framework::owner_lease_renew_result_t> (
          zlink::framework::result_t<
            zlink::framework::owner_lease_renew_result_t>::failure (
            zlink::framework::framework_error_kind_t::internal_failure,
            "owner lease renewal failed"));
    }
};

class fake_location_runtime_query_t final : public location_runtime_query_t
{
  public:
    bool fail_lists = false;
    std::atomic_bool store_healthy{true};
    std::atomic_int status_calls{0};

    zlink::framework::task_t<location_runtime_status_t> get_status () override
    {
        ++status_calls;
        return completed (location_runtime_status_t{.store_healthy = store_healthy.load (),
                                                    .watch_enabled = false,
                                                    .polling_interval =
                                                      std::chrono::milliseconds (10),
                                                    .owner_lease_healthy = true});
    }

    zlink::framework::task_t<location_page_t<location_topology_entry_t>>
    list_topology (location_topology_filter_t, location_page_request_t = {}) override
    {
        if (fail_lists) {
            return failed<location_page_t<location_topology_entry_t>> ();
        }
        return completed (location_page_t<location_topology_entry_t>{
          .items = {location_topology_entry_t{.mesh_name = "mesh-a",
                                              .state = location_topology_state_t::ready,
                                              }}});
    }

    zlink::framework::task_t<location_page_t<location_service_summary_t>>
    list_service_summaries (location_service_summary_filter_t,
                            location_page_request_t = {}) override
    {
        if (fail_lists) {
            return failed<location_page_t<location_service_summary_t>> ();
        }
        return completed (location_page_t<location_service_summary_t>{
          .items = {location_service_summary_t{.mesh_name = "mesh-a",
                                               .total_count = 1,
                                               .ready_count = 1}}});
    }

  private:
    template <typename T> static zlink::framework::task_t<T> completed (T value)
    {
        return zlink::framework::task_t<T> (
          zlink::framework::result_t<T>::success (std::move (value)));
    }

    template <typename T> static zlink::framework::task_t<T> failed ()
    {
        return zlink::framework::task_t<T> (
          zlink::framework::result_t<T>::failure (
            zlink::framework::framework_error_kind_t::internal_failure,
            "location query list failed"));
    }
};

class no_op_stream_session_t final : public zlink::framework::packet_stream_session_t
{
  public:
    zlink::framework::task_t<void> on_connected (zlink::framework::stream_t &) override
    {
        return success ();
    }

    zlink::framework::task_t<void> on_disconnected (zlink::framework::stream_t &) override
    {
        return success ();
    }

    zlink::framework::task_t<void>
    on_error (zlink::framework::stream_t &, const zlink::framework::stream_error_t &) override
    {
        return success ();
    }

    zlink::framework::task_t<void>
    on_packet (zlink::framework::stream_t &,
               const zlink::framework::session_message_context_t &,
               const zlink::message_t &) override
    {
        return success ();
    }

  private:
    static zlink::framework::task_t<void> success ()
    {
        return zlink::framework::task_t<void> (zlink::framework::result_t<void>::success ());
    }
};

class echo_stream_session_t final : public zlink::framework::packet_stream_session_t
{
  public:
    zlink::framework::task_t<void> on_connected (zlink::framework::stream_t &) override
    {
        return success ();
    }

    zlink::framework::task_t<void> on_disconnected (zlink::framework::stream_t &) override
    {
        return success ();
    }

    zlink::framework::task_t<void>
    on_error (zlink::framework::stream_t &, const zlink::framework::stream_error_t &) override
    {
        return success ();
    }

    zlink::framework::task_t<void>
    on_packet (zlink::framework::stream_t &stream,
               const zlink::framework::session_message_context_t &,
               const zlink::message_t &payload) override
    {
        stream.reply_packet (payload).submit ();
        return success ();
    }

  private:
    static zlink::framework::task_t<void> success ()
    {
        return zlink::framework::task_t<void> (zlink::framework::result_t<void>::success ());
    }
};

class stop_after_delay_t final : public zlink::framework::hosted_service_t
{
  public:
    explicit stop_after_delay_t (zlink::framework::app_t &app) : _app (&app) {}

    void start (zlink::framework::service_provider_t &) override
    {
        std::this_thread::sleep_for (std::chrono::milliseconds (50));
        _app->stop ();
    }

    void stop () noexcept override {}

  private:
    zlink::framework::app_t *_app;
};

class stream_roundtrip_client_t final : public zlink::framework::hosted_service_t
{
  public:
    stream_roundtrip_client_t (zlink::framework::app_t &app,
                               std::string endpoint,
                               zlink::framework::detail::stream_runtime_t runtime) :
        _app (&app), _endpoint (std::move (endpoint)), _runtime (std::move (runtime))
    {
    }
    void start (zlink::framework::service_provider_t &) override
    {
        for (int attempt = 0; attempt < 80; ++attempt) {
            try {
                run_once ();
                observed = true;
                break;
            }
            catch (const std::exception &) {
                last_error = std::current_exception ();
                std::this_thread::sleep_for (std::chrono::milliseconds (25));
            }
        }
        _app->stop ();
    }

    void stop () noexcept override {}

    bool observed = false;
    std::exception_ptr last_error;

    std::string last_error_message () const
    {
        if (!last_error) {
            return {};
        }
        try {
            std::rethrow_exception (last_error);
        }
        catch (const std::exception &error) {
            return error.what ();
        }
        catch (...) {
            return "unknown stream roundtrip failure";
        }
    }

  private:
    struct tcp_endpoint_t
    {
        std::string host;
        std::string port;
    };

    static tcp_endpoint_t parse_endpoint (const std::string &endpoint)
    {
        constexpr std::string_view prefix = "tcp://";
        if (endpoint.rfind (prefix, 0) != 0) {
            throw std::runtime_error ("test stream endpoint must be tcp");
        }
        const auto host_start = prefix.size ();
        const auto separator = endpoint.rfind (':');
        if (separator == std::string::npos || separator <= host_start
            || separator + 1 >= endpoint.size ()) {
            throw std::runtime_error ("test stream endpoint is malformed");
        }
        return {endpoint.substr (host_start, separator - host_start),
                endpoint.substr (separator + 1)};
    }

    std::vector<std::uint8_t> make_frame (
      const zlink::framework::detail::stream_header_t &header,
      const zlink::message_t &payload)
    {
        auto encoded_header = _runtime.encode_header (header);
        if (!encoded_header) {
            throw std::runtime_error ("failed to encode test stream header");
        }
        const auto payload_bytes = payload.to_bytes ();
        std::vector<std::uint8_t> frame;
        frame.reserve (6 + encoded_header.value ().size () + payload_bytes.size ());
        const auto header_size = encoded_header.value ().size ();
        frame.push_back (static_cast<std::uint8_t> ((header_size >> 8) & 0xff));
        frame.push_back (static_cast<std::uint8_t> (header_size & 0xff));
        frame.push_back (static_cast<std::uint8_t> ((payload_bytes.size () >> 24) & 0xff));
        frame.push_back (static_cast<std::uint8_t> ((payload_bytes.size () >> 16) & 0xff));
        frame.push_back (static_cast<std::uint8_t> ((payload_bytes.size () >> 8) & 0xff));
        frame.push_back (static_cast<std::uint8_t> (payload_bytes.size () & 0xff));
        frame.insert (frame.end (), encoded_header.value ().begin (),
                      encoded_header.value ().end ());
        frame.insert (frame.end (), payload_bytes.begin (), payload_bytes.end ());
        return frame;
    }

    zlink::message_t read_reply (boost::asio::ip::tcp::socket &socket)
    {
        std::array<std::uint8_t, 6> prefix{};
        boost::asio::read (socket, boost::asio::buffer (prefix));
        const auto header_size = static_cast<std::size_t> ((prefix[0] << 8) | prefix[1]);
        const auto payload_size = (static_cast<std::size_t> (prefix[2]) << 24)
                                  | (static_cast<std::size_t> (prefix[3]) << 16)
                                  | (static_cast<std::size_t> (prefix[4]) << 8)
                                  | static_cast<std::size_t> (prefix[5]);
        std::vector<std::uint8_t> header_bytes (header_size);
        std::vector<std::uint8_t> payload_bytes (payload_size);
        boost::asio::read (socket, boost::asio::buffer (header_bytes));
        boost::asio::read (socket, boost::asio::buffer (payload_bytes));
        auto header = _runtime.decode_header (header_bytes);
        if (!header
            || header.value ().kind ()
                 != zlink::framework::detail::stream_message_kind_t::response
            || header.value ().request_seq () != 91) {
            if (!header) {
                throw std::runtime_error ("failed to decode stream reply header");
            }
            throw std::runtime_error (
              "unexpected stream reply header kind=" + std::to_string (static_cast<int> (header.value ().kind ()))
              + " seq="
              + (header.value ().request_seq () ? std::to_string (*header.value ().request_seq ())
                                                 : std::string ("none"))
              + " packet=" + std::string (header.value ().packet_name ()));
        }
        return zlink::message_t::from (payload_bytes);
    }

    void run_once ()
    {
        const auto endpoint = parse_endpoint (_endpoint);
        boost::asio::io_context io;
        boost::asio::ip::tcp::resolver resolver (io);
        boost::asio::ip::tcp::socket socket (io);
        boost::asio::connect (socket, resolver.resolve (endpoint.host, endpoint.port));
        zlink::framework::detail::stream_header_t header (
          zlink::framework::detail::stream_message_kind_t::request,
          zlink::framework::stream_codec_t::raw,
          zlink::framework::detail::stream_header_flags_t::has_request_seq, 91,
          "stream.roundtrip");
        header.with_correlation_id ("stream-roundtrip");
        auto frame = make_frame (header, zlink::message_t::from (std::string ("stream-payload")));
        boost::asio::write (socket, boost::asio::buffer (frame));
        if (read_reply (socket).to_string () != "stream-payload") {
            throw std::runtime_error ("unexpected stream reply payload");
        }
        boost::system::error_code ignored;
        socket.shutdown (boost::asio::ip::tcp::socket::shutdown_both, ignored);
        socket.close (ignored);
    }

    zlink::framework::app_t *_app;
    std::string _endpoint;
    zlink::framework::detail::stream_runtime_t _runtime;
};

struct options_fixture_t
{
    zlink::framework::service_collection_t services;
    zlink::framework::handler_registry_t handlers;
    zlink::framework::serializer_registry_t serializers;
    zlink::framework::zlink_builder_t zlink;
    zlink::framework::zlink_framework_options_t make_options ()
    {
        return zlink::framework::zlink_framework_options_t (
          services, handlers, serializers, zlink);
    }
};

struct auto_connect_request_t
{
    // This is an ordinary application packet name. The ClientServer reply
    // header, not a reserved packet-name sentinel, determines success.
    static constexpr const char *packet_name =
      "$zlink.client-server.error";
    int value{};
};

struct auto_connect_reply_t
{
    int value{};
};

struct auto_connect_event_t
{
    int value{};
};

struct blocked_auto_connect_event_t
{
    static constexpr const char *packet_name =
      "blocked-auto-connect-event";
    int value{};
};

void to_json (nlohmann::json &json, const auto_connect_request_t &value)
{
    json = nlohmann::json{{"value", value.value}};
}

void from_json (const nlohmann::json &json, auto_connect_request_t &value)
{
    value.value = json.at ("value").get<int> ();
}

void to_json (nlohmann::json &json, const auto_connect_reply_t &value)
{
    json = nlohmann::json{{"value", value.value}};
}

void from_json (const nlohmann::json &json, auto_connect_reply_t &value)
{
    value.value = json.at ("value").get<int> ();
}

void to_json (nlohmann::json &json, const auto_connect_event_t &value)
{
    json = nlohmann::json{{"value", value.value}};
}

void from_json (const nlohmann::json &json, auto_connect_event_t &value)
{
    value.value = json.at ("value").get<int> ();
}

void to_json (
  nlohmann::json &json,
  const blocked_auto_connect_event_t &value)
{
    json = nlohmann::json{{"value", value.value}};
}

void from_json (
  const nlohmann::json &json,
  blocked_auto_connect_event_t &value)
{
    value.value = json.at ("value").get<int> ();
}

class auto_connect_request_handler_t
{
  public:
    using request_type = auto_connect_request_t;
    using reply_type = auto_connect_reply_t;

    auto_connect_reply_t handle (const auto_connect_request_t &request)
    {
        return auto_connect_reply_t{request.value + 1000};
    }
};

struct automatic_handler_scope_dependency_t
{
    automatic_handler_scope_dependency_t () { ++created; }
    ~automatic_handler_scope_dependency_t () { ++destroyed; }

    inline static std::atomic_int created{0};
    inline static std::atomic_int destroyed{0};
};

class automatic_handler_scope_filter_t
{
  public:
    using dependency_types =
      zlink::framework::dependency_list_t<
        automatic_handler_scope_dependency_t>;

    explicit automatic_handler_scope_filter_t (
      automatic_handler_scope_dependency_t &dependency) :
        _dependency (dependency)
    {
    }

    zlink::framework::task_t<void>
    invoke (
      const zlink::framework::handler_filter_context_t &context,
      zlink::framework::handler_next_t next)
    {
        last_filter_dependency.store (
          &_dependency, std::memory_order_release);
        if (context.packet_name
            == blocked_auto_connect_event_t::packet_name) {
            ++blocked_fanout_dispatches;
            co_return;
        }
        if (reject_requests.load (std::memory_order_acquire))
            co_return;
        co_await next ();
        co_return;
    }

    inline static std::atomic_bool reject_requests{false};
    inline static std::atomic<void *> last_filter_dependency{nullptr};
    inline static std::atomic_int blocked_fanout_dispatches{0};

  private:
    automatic_handler_scope_dependency_t &_dependency;
};

class automatic_scoped_request_handler_t
{
  public:
    using request_type = auto_connect_request_t;
    using reply_type = auto_connect_reply_t;
    using dependency_types =
      zlink::framework::dependency_list_t<
        automatic_handler_scope_dependency_t>;

    explicit automatic_scoped_request_handler_t (
      automatic_handler_scope_dependency_t &dependency) :
        _dependency (dependency)
    {
    }

    auto_connect_reply_t handle (
      const auto_connect_request_t &request)
    {
        same_scope.store (
          automatic_handler_scope_filter_t::last_filter_dependency.load (
            std::memory_order_acquire)
            == &_dependency,
          std::memory_order_release);
        ++invocations;
        return auto_connect_reply_t{request.value + 1000};
    }

    inline static std::atomic_bool same_scope{false};
    inline static std::atomic_int invocations{0};

  private:
    automatic_handler_scope_dependency_t &_dependency;
};

class auto_connect_event_handler_t
{
  public:
    using event_type = auto_connect_event_t;
    static constexpr const char *topic_name = "profile.changed";
    using dependency_types =
      zlink::framework::dependency_list_t<
        automatic_handler_scope_dependency_t>;

    explicit auto_connect_event_handler_t (
      automatic_handler_scope_dependency_t &dependency) :
        _dependency (dependency)
    {
    }

    static inline std::atomic<int> observed_count{0};
    static inline std::atomic<int> observed_value{0};

    void handle (const auto_connect_event_t &event)
    {
        same_scope.store (
          automatic_handler_scope_filter_t::last_filter_dependency.load (
            std::memory_order_acquire)
            == &_dependency,
          std::memory_order_release);
        observed_value.store (event.value, std::memory_order_release);
        observed_count.fetch_add (1, std::memory_order_acq_rel);
    }

    static inline std::atomic_bool same_scope{false};

  private:
    automatic_handler_scope_dependency_t &_dependency;
};

class blocked_auto_connect_event_handler_t
{
  public:
    using event_type = blocked_auto_connect_event_t;
    static constexpr const char *topic_name = "profile.changed";
    using dependency_types =
      zlink::framework::dependency_list_t<
        automatic_handler_scope_dependency_t>;

    explicit blocked_auto_connect_event_handler_t (
      automatic_handler_scope_dependency_t &) {}

    void handle (const blocked_auto_connect_event_t &)
    {
        ++observed_count;
    }

    static inline std::atomic_int observed_count{0};
};

class auto_connect_request_client_t final : public zlink::framework::hosted_service_t
{
  public:
    explicit auto_connect_request_client_t (zlink::framework::app_t &app) : _app (&app) {}

    void start (zlink::framework::service_provider_t &) override
    {
        auto client = _app->advanced ().zlink ().request_client ("orders");
        for (int attempt = 0; attempt < 40; ++attempt) {
            auto reply = client.request (auto_connect_request_t{17})
                           .timeout (std::chrono::milliseconds (500))
                           .submit<auto_connect_reply_t> ()
                           .result ();
            if (reply && reply.value ().value == 1017) {
                observed = true;
                break;
            }
            if (!reply && reply.error ()) {
                last_error = reply.error ()->what ();
            }
            std::this_thread::sleep_for (std::chrono::milliseconds (25));
        }
        _app->stop ();
    }

    void stop () noexcept override {}

    bool observed = false;
    std::string last_error;

  private:
    zlink::framework::app_t *_app;
};

class missing_auto_connect_request_client_t final
    : public zlink::framework::hosted_service_t
{
  public:
    explicit missing_auto_connect_request_client_t (
      zlink::framework::app_t &app) :
        _app (&app)
    {
    }
    void start (zlink::framework::service_provider_t &) override
    {
        auto reply = _app->advanced ().zlink ().request_client ("orders")
                       .request (auto_connect_request_t{17})
                       .timeout (std::chrono::milliseconds (50))
                       .submit<auto_connect_reply_t> ()
                       .result ();
        if (!reply) {
            observed_error = reply.error_kind ();
        }
        _app->stop ();
    }

    void stop () noexcept override {}

    std::optional<zlink::framework::framework_error_kind_t> observed_error;

  private:
    zlink::framework::app_t *_app;
};

class rejected_auto_connect_request_client_t final
    : public zlink::framework::hosted_service_t
{
  public:
    explicit rejected_auto_connect_request_client_t (
      zlink::framework::app_t &app) :
        _app (&app)
    {
    }

    void start (zlink::framework::service_provider_t &) override
    {
        auto client =
          _app->advanced ().zlink ().request_client ("orders");
        for (int attempt = 0; attempt < 40; ++attempt) {
            auto reply =
              client.request (auto_connect_request_t{17})
                .timeout (std::chrono::milliseconds (500))
                .submit<auto_connect_reply_t> ()
                .result ();
            if (!reply
                && reply.error_kind ()
                     == zlink::framework::framework_error_kind_t::rejected) {
                observed_rejected = true;
                break;
            }
            std::this_thread::sleep_for (
              std::chrono::milliseconds (25));
        }
        _app->stop ();
    }

    void stop () noexcept override {}

    bool observed_rejected = false;

  private:
    zlink::framework::app_t *_app;
};

class actor_free_user_spot_contract_t
    : public zlink::framework::spot_t<zlink::framework::actor_t>
{
  public:
    zlink::framework::task_t<zlink::framework::spot_actor_join_result_t>
    on_actor_join (std::string_view,
                   const zlink::framework::message_t &) override
    {
        co_return zlink::framework::spot_actor_join_result_t::reject ();
    }
    zlink::framework::task_t<void>
    on_actor_joined (zlink::framework::actor_t &) override
    {
        co_return;
    }
    zlink::framework::task_t<void>
    on_leave_actor (zlink::framework::actor_t &) override
    {
        co_return;
    }
};

class local_user_spot_t final
    : public actor_free_user_spot_contract_t
{
  public:
    explicit local_user_spot_t (zlink::framework::spot_context_t context) :
        _context (std::move (context))
    {
    }
    zlink::framework::spot_context_t &context () noexcept override { return _context; }
    const zlink::framework::spot_context_t &context () const noexcept override
    {
        return _context;
    }
    void configure () override {}

  private:
    zlink::framework::spot_context_t _context;
};

class context_owned_user_spot_t final
    : public actor_free_user_spot_contract_t
{
  public:
    explicit context_owned_user_spot_t (
      zlink::framework::spot_context_t context,
      std::shared_ptr<std::atomic<int>> destruction_count = {}) :
        _context (std::move (context)),
        _destruction_count (std::move (destruction_count))
    {
    }

    ~context_owned_user_spot_t () override
    {
        if (_destruction_count)
            _destruction_count->fetch_add (1, std::memory_order_relaxed);
    }

    zlink::framework::spot_context_t &context () noexcept override { return _context; }
    const zlink::framework::spot_context_t &context () const noexcept override
    {
        return _context;
    }
    void configure () override {}

  private:
    zlink::framework::spot_context_t _context;
    std::shared_ptr<std::atomic<int>> _destruction_count;
};

class occupied_user_spot_t final
    : public actor_free_user_spot_contract_t
{
  public:
    explicit occupied_user_spot_t (zlink::framework::spot_context_t context) :
        _context (std::move (context))
    {
    }
    zlink::framework::spot_context_t &context () noexcept override { return _context; }
    const zlink::framework::spot_context_t &context () const noexcept override
    {
        return _context;
    }
    void configure () override {}

  private:
    zlink::framework::spot_context_t _context;
};

class failing_user_spot_t final
    : public actor_free_user_spot_contract_t
{
  public:
    explicit failing_user_spot_t (zlink::framework::spot_context_t context) :
        _context (std::move (context))
    {
        throw std::runtime_error (
          "intentional User Spot materialization failure");
    }
    zlink::framework::spot_context_t &context () noexcept override { return _context; }
    const zlink::framework::spot_context_t &context () const noexcept override
    {
        return _context;
    }
    void configure () override {}

  private:
    zlink::framework::spot_context_t _context;
};

class user_spot_manager_client_t final
    : public zlink::framework::hosted_service_t
{
  public:
    explicit user_spot_manager_client_t (
      zlink::framework::app_t &app) :
        _app (&app)
    {
    }

    void start (
      zlink::framework::service_provider_t &services) override
    {
        try {
            auto &manager =
              services.get_required<zlink::framework::spot_manager_t> ();
            const auto rid =
              zlink::framework::spot_id_t (
                "local-user-spot");
            auto created =
              manager.get_or_create (rid, "room")
                .timeout (std::chrono::seconds (2))
                .submit ()
                .result ();
            if (!created) {
                last_error =
                  created.error () ? created.error ()->what ()
                                   : "User Spot create failed";
                _app->stop ();
                return;
            }
            auto found = manager.find (rid).result ();
            if (!found || !found.value ()) {
                last_error =
                  found.error () ? found.error ()->what ()
                                 : "User Spot find failed";
                _app->stop ();
                return;
            }
            auto closed =
              manager.close (*found.value ()).result ();
            if (!closed || !closed.value ()) {
                last_error =
                  closed.error () ? closed.error ()->what ()
                                  : "User Spot close failed";
                _app->stop ();
                return;
            }
            observed = true;
        }
        catch (const std::exception &error) {
            last_error = error.what ();
        }
        _app->stop ();
    }

    void stop () noexcept override {}

    bool observed = false;
    std::string last_error;

  private:
    zlink::framework::app_t *_app;
};

class generated_user_spot_collision_client_t final
    : public zlink::framework::hosted_service_t
{
  public:
    explicit generated_user_spot_collision_client_t (
      zlink::framework::app_t &app,
      std::shared_ptr<test_location_repository_t> store) :
        _app (&app),
        _store (std::move (store))
    {
    }

    void start (
      zlink::framework::service_provider_t &services) override
    {
        try {
            auto &manager =
              services.get_required<
                zlink::framework::spot_manager_t> ();
            const auto collision =
              zlink::framework::spot_id_t (
                "user:spot-collision-node:1");
            auto occupied =
              manager.get_or_create (collision, "occupied")
                .timeout (std::chrono::seconds (2))
                .submit ()
                .result ();
            if (!occupied) {
                last_error =
                  occupied.error ()
                    ? occupied.error ()->what ()
                    : "Failed to create the collision identity";
                _app->stop ();
                return;
            }
            _store->force_reserve_conflict.store (
              true, std::memory_order_release);
            auto generated =
              manager.create ("room")
                .timeout (std::chrono::seconds (2))
                .submit ()
                .result ();
            if (generated
                || !generated.error ()
                || generated.error ()->kind ()
                     != zlink::framework::
                          framework_error_kind_t::already_exists) {
                last_error =
                  "Generated User Spot collision was not rejected immediately";
                _app->stop ();
                return;
            }
            auto existing =
              manager.get_or_create (collision, "occupied")
                .timeout (std::chrono::seconds (2))
                .submit ()
                .result ();
            if (!existing
                || existing.value ().state
                     != zlink::framework::spot_create_state_t::
                          existing
                || existing.value ().spot.object_generation ()
                     != occupied.value ().spot.object_generation ()) {
                last_error =
                  "GetOrCreate did not preserve the caller RID incarnation";
                _app->stop ();
                return;
            }
            auto mismatch =
              manager.get_or_create (collision, "room")
                .timeout (std::chrono::seconds (2))
                .submit ()
                .result ();
            if (mismatch
                || !mismatch.error ()
                || mismatch.error ()->kind ()
                     != zlink::framework::
                          framework_error_kind_t::type_mismatch) {
                last_error =
                  "GetOrCreate changed caller RID type mismatch semantics";
                _app->stop ();
                return;
            }
            observed = true;
        }
        catch (const std::exception &error) {
            last_error = error.what ();
        }
        _app->stop ();
    }

    void stop () noexcept override {}

    bool observed = false;
    std::string last_error;

  private:
    zlink::framework::app_t *_app;
    std::shared_ptr<test_location_repository_t> _store;
};

class source_cleanup_client_t final
    : public zlink::framework::hosted_service_t
{
  public:
    explicit source_cleanup_client_t (
      zlink::framework::app_t &app) :
        _app (&app)
    {
    }

    void start (
      zlink::framework::service_provider_t &services) override
    {
        auto &manager =
          services.get_required<
            zlink::framework::spot_manager_t> ();
        const auto result =
          manager
            .get_or_create (
              zlink::framework::spot_id_t (
                "source-cleanup"),
              "failing")
            .timeout (std::chrono::seconds (2))
            .submit ()
            .result ();
        observed =
          !result && result.error ()
          && result.error ()->kind ()
               == zlink::framework::
                    framework_error_kind_t::internal_failure;
        if (!observed)
            last_error =
              result.error () ? result.error ()->what ()
                              : "User Spot failure was not preserved";
        _app->stop ();
    }

    void stop () noexcept override {}

    bool observed = false;
    std::string last_error;

  private:
    zlink::framework::app_t *_app;
};

class auto_connect_publish_client_t final : public zlink::framework::hosted_service_t
{
  public:
    explicit auto_connect_publish_client_t (zlink::framework::app_t &app) : _app (&app) {}

    void start (zlink::framework::service_provider_t &) override
    {
        auto publisher = _app->advanced ().zlink ().publisher ();
        for (int attempt = 0; attempt < 80; ++attempt) {
            try {
                publisher
                  .publish (
                    "events", "profile.changed",
                    blocked_auto_connect_event_t{attempt + 1})
                  .submit ();
            }
            catch (const std::exception &error) {
                last_error = error.what ();
            }
            if (automatic_handler_scope_filter_t::
                  blocked_fanout_dispatches.load (
                    std::memory_order_acquire)
                > 0)
                break;
            std::this_thread::sleep_for (
              std::chrono::milliseconds (25));
        }
        for (int attempt = 0; attempt < 80; ++attempt) {
            try {
                publisher.publish ("events", "profile.changed", auto_connect_event_t{attempt + 1})
                  .submit ();
            }
            catch (const std::exception &error) {
                last_error = error.what ();
            }
            if (auto_connect_event_handler_t::observed_count.load (std::memory_order_acquire) > 0) {
                observed = true;
                break;
            }
            std::this_thread::sleep_for (std::chrono::milliseconds (25));
        }
        _app->stop ();
    }

    void stop () noexcept override {}

    bool observed = false;
    std::string last_error;

  private:
    zlink::framework::app_t *_app;
};

template <typename Store>
location_owner_token_t claim_test_owner (
  Store &store,
  std::string owner_id,
  std::chrono::milliseconds ttl = std::chrono::seconds (30))
{
    const auto claimed =
      store.claim_owner_lease (owner_id, ttl)
        .result ()
        .value ();
    const auto *value =
      std::get_if<zlink::framework::owner_lease_claimed_t> (
        &claimed);
    if (value == nullptr)
        throw std::runtime_error (
          "test owner lease claim failed");
    return value->token;
}

bool wait_until (const std::function<bool ()> &predicate)
{
    for (int attempt = 0; attempt < 40; ++attempt) {
        if (predicate ()) {
            return true;
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (25));
    }
    return predicate ();
}

std::uint32_t current_process_id () noexcept
{
#ifdef _WIN32
    return static_cast<std::uint32_t> (_getpid ());
#else
    return static_cast<std::uint32_t> (getpid ());
#endif
}

std::uint16_t bindable_loopback_port (std::uint16_t base_port)
{
    const auto offset = static_cast<std::uint16_t> ((current_process_id () % 1000U) * 11U);
    const auto first = static_cast<std::uint16_t> (base_port + offset);
#ifdef _WIN32
    return first;
#else
    for (std::uint16_t attempt = 0; attempt < 200; ++attempt) {
        const auto candidate = static_cast<std::uint16_t> (first + attempt * 13U);
        const int descriptor = ::socket (AF_INET, SOCK_STREAM, 0);
        if (descriptor < 0) {
            return first;
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons (candidate);
        address.sin_addr.s_addr = htonl (INADDR_LOOPBACK);
        const bool bindable =
          ::bind (descriptor, reinterpret_cast<sockaddr *> (&address), sizeof (address)) == 0;
        ::close (descriptor);
        if (bindable) {
            return candidate;
        }
    }
    return first;
#endif
}

TEST (ZLinkFrameworkStoreLocationResolvers, ResolvesSpotAddressFromStore)
{
    test_location_repository_t store;
    (void) claim_test_owner (store, "owner-a");
    const auto owner = live_owner_token (store, "owner-a");
    store.set_authority (
      "zla1:s:6:spot-a",
      zlink::framework::authority_snapshot_t{
        .store_version = "1",
        .payload = user_spot_authority_payload ("spot-a", 1),
        .object_generation = 1,
        .authority_owner_generation = 1,
        .owner = owner,
        .allocation = {.state = zlink::framework::placement_allocation_state_t::active,
                       .object_kind = zlink::framework::placement_object_kind_t::user_spot,
                       .stable_type = "play",
                       .target = {.mesh_name = "mesh-a",
                                  .node_rid = zlink::framework::node_rid_t::from_string ("node-a"),
                                  .node_lifecycle_generation = 1,
                                  .owner = owner}}});
    store_location_resolvers_t resolvers (store);

    const auto address =
      resolvers.resolve_spot_address ("mesh-a", "spot-a")
        .result ()
        .value ();

    ASSERT_TRUE (address.has_value ());
    EXPECT_EQ ("mesh-a", address->mesh_name);
    EXPECT_EQ ("node-a", address->node_rid.to_string ());
    EXPECT_EQ ("spot-a", address->spot_id);
}

TEST (ZLinkFrameworkStoreLocationResolvers, DirectReadyRouteUsesPositiveCacheOnly)
{
    test_location_repository_t store;
    (void) claim_test_owner (store, "owner-cache");
    const auto owner = live_owner_token (store, "owner-cache");
    auto authority = zlink::framework::authority_snapshot_t{
        .store_version = "10",
        .payload = user_spot_authority_payload ("spot-cache", 1),
        .object_generation = 1,
        .authority_owner_generation = 1,
        .owner = owner,
        .allocation = zlink::framework::placement_allocation_t{
          .state = zlink::framework::placement_allocation_state_t::active,
          .object_kind = zlink::framework::placement_object_kind_t::user_spot,
          .stable_type = "play",
          .target = zlink::framework::object_creation_target_t{
            .mesh_name = "mesh-cache",
            .node_rid = zlink::framework::node_rid_t::from_string ("node-cache"),
            .node_lifecycle_generation = 1,
            .owner = owner}}};
    store.set_authority ("zla1:s:10:spot-cache", authority);
    location_options_t options;
    options.route_cache_max_age = std::chrono::seconds (1);
    store_location_resolvers_t resolvers (store, options);

    ASSERT_TRUE (resolvers.resolve_spot_address ({}, "spot-cache").result ().value ());
    ASSERT_TRUE (resolvers.resolve_spot_address ({}, "spot-cache").result ().value ());
    EXPECT_EQ (0u, store.resolve_spot_count.load ());

    authority.store_version = "11";
    authority.authority_owner_generation = 2;
    store.set_authority ("zla1:s:10:spot-cache", authority);
    resolvers.observe_spot_authority_version ("spot-cache", "11", 1, 2);
    ASSERT_TRUE (resolvers.resolve_spot_address ({}, "spot-cache").result ().value ());
    EXPECT_EQ (0u, store.resolve_spot_count.load ());

    resolvers.invalidate_all_routes_after_store_recovery ();
    ASSERT_TRUE (resolvers.resolve_spot_address ({}, "spot-cache").result ().value ());
    EXPECT_EQ (0u, store.resolve_spot_count.load ());

    EXPECT_FALSE (resolvers.resolve_spot_address ({}, "missing").result ().value ());
    EXPECT_FALSE (resolvers.resolve_spot_address ({}, "missing").result ().value ());
    EXPECT_EQ (0u, store.resolve_spot_count.load ());
}

TEST (ZLinkFrameworkStoreLocationResolvers, AddLocationStoreRegistersOpaqueProvider)
{
    options_fixture_t fixture;
    auto options = fixture.make_options ();
    auto store = std::make_shared<in_memory_location_store_t> ();

    options.add_location_store (store);
    options.apply ();

    auto provider = fixture.services.build_provider ();
    EXPECT_EQ (
      store.get (),
      &provider.get_required<
        zlink::framework::location_store_t> ());
}

TEST (ZLinkFrameworkStoreLocationResolvers, AppFrameworkUsesConfiguredLocationStore)
{
    auto app = zlink::framework::app_t::create ();
    auto store = std::make_shared<in_memory_location_store_t> ();

    app.add_zlink_framework ([&] (zlink::framework::zlink_framework_options_t &options) {
        options.add_location_store (store);
        options.configure_locations ().owner_lease_renew_interval = std::chrono::milliseconds (25);
    });

    auto provider = app.advanced ().services ().build_provider ();
    EXPECT_EQ (
      store.get (),
      &provider.get_required<
        zlink::framework::location_store_t> ());
    EXPECT_EQ (std::chrono::milliseconds (25),
               provider.get_required<zlink::framework::runtime::location_runtime_t> ()
                 .options ()
                 .owner_lease_renew_interval);
}

TEST (ZLinkFrameworkStoreLocationResolvers, DuplicateLocationStoreRegistrationIsRejected)
{
    options_fixture_t fixture;
    auto options = fixture.make_options ();
    auto first = std::make_shared<in_memory_location_store_t> ();
    auto second = std::make_shared<in_memory_location_store_t> ();

    options.add_location_store (first);

    EXPECT_THROW (
      options.add_location_store (second),
      zlink::framework::framework_exception_t);
}

TEST (ZLinkFrameworkStoreLocationResolvers, RejectsInvalidRoutingAndRelocationLimitsBeforeApply)
{
    {
        options_fixture_t fixture;
        auto options = fixture.make_options ();
        options.configure_locations ().route_cache_max_age = std::chrono::seconds (26);
        options.configure_locations ().message_follow_duration = std::chrono::seconds (30);
        EXPECT_THROW (options.apply (), zlink::framework::framework_exception_t);
    }
    {
        options_fixture_t fixture;
        auto options = fixture.make_options ();
        options.configure_locations ().max_active_outbound_relocations = 0;
        EXPECT_THROW (options.apply (), zlink::framework::framework_exception_t);
    }
    {
        options_fixture_t fixture;
        auto options = fixture.make_options ();
        options.configure_locations ().owner_lease_renew_interval = std::chrono::seconds (7);
        EXPECT_THROW (options.apply (), zlink::framework::framework_exception_t);
    }
}

TEST (ZLinkFrameworkStoreLocationResolvers,
      FanoutAutomaticAndManualSubscriberSourcesFailConfiguration)
{
    options_fixture_t fixture;
    auto options = fixture.make_options ();

    options.add_fanout_channel ("events")
      .enable_subscriber ()
      .connect ("tcp://127.0.0.1:7001");

    try {
        options.apply ();
        FAIL () << "mixed fanout subscriber sources must fail configuration";
    }
    catch (const zlink::framework::framework_exception_t &error) {
        EXPECT_EQ (zlink::framework::framework_error_kind_t::protocol_error,
                   error.kind ());
        EXPECT_NE (std::string::npos,
                   std::string (error.what ()).find (
                     "cannot combine automatic discovery with manual subscriber endpoints"));
    }
}

TEST (ZLinkFrameworkStoreLocationResolvers, ResolvesSpotAddressByGlobalIdAcrossMeshes)
{
    test_location_repository_t store;
    (void) claim_test_owner (store, "owner-a");
    const auto owner = live_owner_token (store, "owner-a");
    auto authority = zlink::framework::authority_snapshot_t{
      .store_version = "1",
      .payload = user_spot_authority_payload ("spot-b", 41),
      .object_generation = 41,
      .authority_owner_generation = 1,
      .owner = owner,
      .allocation = {.state = zlink::framework::placement_allocation_state_t::active,
                     .object_kind = zlink::framework::placement_object_kind_t::user_spot,
                     .stable_type = "play",
                     .target = {.mesh_name = "mesh-b",
                                .node_rid = zlink::framework::node_rid_t::from_string ("node-a"),
                                .node_lifecycle_generation = 1,
                                .owner = owner}}};
    store.set_authority ("zla1:s:6:spot-b", authority);
    location_options_t options;
    options.route_cache_max_age = std::chrono::milliseconds::zero ();
    store_location_resolvers_t resolvers (store, options);

    /* No mesh name in the lookup: the rid is searched across every mesh. */
    const auto initial = resolvers.resolve_spot_address ({}, "spot-b").result ().value ();
    ASSERT_TRUE (initial.has_value ());
    EXPECT_EQ ("spot-b", initial->spot_id);
    EXPECT_EQ (41u, initial->spot_generation);

    authority.store_version = "2";
    authority.payload = user_spot_authority_payload ("spot-b", 42);
    authority.object_generation = 42;
    store.set_authority ("zla1:s:6:spot-b", authority);
    const auto renewed = resolvers.resolve_spot_address ({}, "spot-b").result ().value ();
    ASSERT_TRUE (renewed.has_value ());
    EXPECT_EQ (42u, renewed->spot_generation);

    const auto missing =
      resolvers.resolve_spot_address ({}, "missing").result ().value ();
    EXPECT_FALSE (missing.has_value ());
}

TEST (ZLinkFrameworkStoreLocationResolvers, ResolvesActorAddress)
{
    test_location_repository_t store;
    (void) claim_test_owner (store, "owner-a");
    const auto owner = live_owner_token (store, "owner-a");
    store.set_authority (
      "zla1:a:7:actor-b",
      zlink::framework::authority_snapshot_t{
        .store_version = "1",
        .payload = zlink::framework::runtime::encode_actor_authority_payload (
          zlink::framework::detail::actor_ref_access_t::make (
            zlink::framework::node_rid_t::from_string ("node-a"),
            "player", "actor-b", 1),
          "spot-b", 1),
        .object_generation = 1,
        .authority_owner_generation = 1,
        .owner = owner,
        .allocation = {.state = zlink::framework::placement_allocation_state_t::active,
                       .object_kind = zlink::framework::placement_object_kind_t::actor,
                       .stable_type = "player",
                       .target = {.mesh_name = "mesh-b",
                                  .node_rid = zlink::framework::node_rid_t::from_string ("node-a"),
                                  .node_lifecycle_generation = 1,
                                  .owner = owner}}});
    store_location_resolvers_t resolvers (store, {},
      std::make_shared<zlink::framework::runtime::actor_location_observer_t> (), "mesh-b");

    const auto address = resolvers.resolve_actor_address ("actor-b").result ().value ();
    ASSERT_TRUE (address.has_value ());
    EXPECT_EQ ("spot-b", address->spot_id);

    const auto cached = resolvers.resolve_actor_address ("actor-b").result ().value ();
    ASSERT_TRUE (cached.has_value ());
    EXPECT_EQ (1u, store.resolve_actor_count.load ());
    EXPECT_FALSE (resolvers.invalidate_actor_address_if_matches (
      "actor-b",
      zlink::framework::runtime::spot_address_t{
        cached->mesh_name,
        cached->node_rid,
        cached->spot_id,
        cached->spot_generation,
        cached->store_version,
        cached->object_generation,
        cached->authority_owner_generation,
        cached->owner,
        cached->node_generation + 1}));
    EXPECT_TRUE (resolvers.invalidate_actor_address_if_matches (
      "actor-b", *cached));
    const auto reloaded = resolvers.resolve_actor_address ("actor-b").result ().value ();
    ASSERT_TRUE (reloaded.has_value ());
    EXPECT_EQ (cached->object_generation, reloaded->object_generation);
    EXPECT_EQ (2u, store.resolve_actor_count.load ());

    /* Destroy removes the authority, so a cached route must be discarded even
     * when the replacement incarnation can reuse the same generation. */
    resolvers.invalidate_actor_address ("actor-b");
    const auto after_destroy =
      resolvers.resolve_actor_address ("actor-b").result ().value ();
    ASSERT_TRUE (after_destroy.has_value ());
    EXPECT_EQ (3u, store.resolve_actor_count.load ());

    const auto missing = resolvers.resolve_actor_address ("nobody").result ().value ();
    EXPECT_FALSE (missing.has_value ());
}

TEST (ZLinkFrameworkStoreLocationResolvers, RejectsMalformedActorAuthorityPayload)
{
    test_location_repository_t store;
    location_options_t options;
    options.route_cache_max_age = std::chrono::milliseconds::zero ();
    store_location_resolvers_t resolvers (store, options);
    (void) claim_test_owner (store, "owner");
    const auto owner = live_owner_token (store, "owner");
    auto authority = zlink::framework::authority_snapshot_t{
      .store_version = "1",
      .payload = {std::byte{1}},
      .object_generation = 1,
      .authority_owner_generation = 1,
      .owner = owner,
      .allocation = {.state = zlink::framework::placement_allocation_state_t::active,
                     .object_kind = zlink::framework::placement_object_kind_t::actor,
                     .stable_type = "player",
                     .target = {.mesh_name = "mesh",
                                .node_rid = zlink::framework::node_rid_t::from_string ("node"),
                                .node_lifecycle_generation = 1,
                                .owner = owner}}};
    store.set_authority ("zla1:a:14:actor-observed", authority);
    EXPECT_FALSE (resolvers.resolve_actor_address ("actor-observed").result ().value ());
    authority.payload = zlink::framework::runtime::encode_actor_authority_payload (
      zlink::framework::detail::actor_ref_access_t::make (
        zlink::framework::node_rid_t::from_string ("node"),
        "player", "actor-observed", 3),
      "spot-observed", 2);
    store.set_authority ("zla1:a:14:actor-observed", authority);
    const auto committed = resolvers.resolve_actor_address ("actor-observed").result ().value ();
    ASSERT_TRUE (committed);
    EXPECT_EQ ("node", committed->node_rid.to_string ());
    EXPECT_EQ ("spot-observed", committed->spot_id);
}

TEST (ZLinkFrameworkStoreLocationResolvers, ResolverFiltersExpiredMeshNodeDescriptors)
{
    in_memory_location_repository_t store;
    seed_mesh_node (store, "owner-live", "mesh-a", "node-live",
                    "tcp://127.0.0.1:7001");
    const auto expired = claim_test_owner (
      store, "owner-expired", std::chrono::milliseconds (20));
    ASSERT_EQ (
      location_write_status_t::stored,
      store.update_mesh_node (
        zlink::framework::mesh_node_descriptor_t{
          .mesh_name = "mesh-a",
          .rid = zlink::routing_id_t::from ("node-expired"),
          .lifecycle_generation = 1,
          .descriptor_revision = 1,
          .endpoint = "tcp://127.0.0.1:7002",
          .application_version = 1,
          .object_role = zlink::framework::object_role_t::server,
          .state = zlink::framework::framework_runtime_state_t::serving,
          .security_identity = "test",
          .owner_id = expired.owner_id,
          .lease_generation = expired.lease_generation},
        location_write_intent_t::new_claim).result ().value ().status);
    std::this_thread::sleep_for (std::chrono::milliseconds (25));

    const auto raw_rows = store.list_mesh_nodes ("mesh-a").result ().value ().items;
    ASSERT_EQ (2u, raw_rows.size ());

    location_options_t options;
    options.polling_interval = std::chrono::milliseconds::zero ();
    zlink::framework::runtime::live_location_reader_t live_reader (store, options);
    const auto live_rows =
      live_reader.list_mesh_nodes ("mesh-a").result ().value ().items;
    ASSERT_EQ (1u, live_rows.size ());
    EXPECT_EQ ("node-live", live_rows.front ().rid.to_string ());

    store_location_resolvers_t resolvers (live_reader);
    const auto resolver_rows =
      resolvers.list_live_mesh_nodes ("mesh-a").result ().value ();

    ASSERT_EQ (1u, resolver_rows.size ());
    EXPECT_EQ ("node-live", resolver_rows.front ().rid.to_string ());

    location_runtime_t runtime (store, options, "owner-query");
    store_location_runtime_query_t query (live_reader, runtime, options);
    const auto query_rows =
      query
        .list_topology (
          location_topology_filter_t{.mesh_name = "mesh-a"})
        .result ()
        .value ();

    ASSERT_EQ (1u, query_rows.items.size ());
}

TEST (ZLinkFrameworkStoreLocationResolvers, RuntimeQueryReportsHealthyStoreStatus)
{
    in_memory_location_repository_t store;
    location_options_t options;
    options.polling_interval = std::chrono::milliseconds (125);
    location_runtime_t runtime (store, options, "owner-a");
    runtime.start (zlink::routing_id_t::from ("node-a"));
    store_location_runtime_query_t query (store, runtime, options);

    const auto status = query.get_status ().result ().value ();

    EXPECT_TRUE (status.store_healthy);
    EXPECT_FALSE (status.watch_enabled);
    EXPECT_EQ (std::chrono::milliseconds (125), status.polling_interval);
    EXPECT_TRUE (status.owner_lease_healthy);
    EXPECT_TRUE (status.owner_lease_renewed_at.has_value ());
    runtime.stop ();
    EXPECT_TRUE (status.last_refresh_at.has_value ());
    EXPECT_FALSE (status.last_error.has_value ());
}

TEST (ZLinkFrameworkStoreLocationResolvers, RuntimeQueryReportsStoreFailureAsStatus)
{
    failing_owner_lease_store_t store;
    location_options_t options;
    location_runtime_t runtime (store, options, "owner-a");
    runtime.start (zlink::routing_id_t::from ("node-a"));
    runtime.renew_owner_lease_once ();
    store_location_runtime_query_t query (store, runtime, options);

    const auto status = query.get_status ().result ().value ();

    EXPECT_FALSE (status.store_healthy);
    EXPECT_FALSE (status.owner_lease_healthy);
    ASSERT_TRUE (status.last_error.has_value ());
    EXPECT_NE (std::string::npos, status.last_error->find ("owner lease renewal failed"));
    runtime.stop ();
}

TEST (ZLinkFrameworkStoreLocationResolvers, RuntimeQueryProjectsMeshNodeDescriptors)
{
    in_memory_location_repository_t store;
    const auto owner = claim_test_owner (store, "owner-a");
    for (const auto &[rid, state] :
         std::vector<std::pair<std::string, zlink::framework::framework_runtime_state_t>>{
           {"node-a", zlink::framework::framework_runtime_state_t::serving},
           {"node-b", zlink::framework::framework_runtime_state_t::draining}}) {
        ASSERT_EQ (location_write_status_t::stored,
                   store.update_mesh_node (
                     zlink::framework::mesh_node_descriptor_t{
                       .mesh_name = "mesh-a",
                       .rid = zlink::routing_id_t::from (rid),
                       .lifecycle_generation = 1,
                       .descriptor_revision = 1,
                       .endpoint = "tcp://127.0.0.1:7001",
                       .application_version = 1,
                       .object_role = zlink::framework::object_role_t::server,
                       .capacity = {.actors = {.limit = 128},
                                    .spots = {.limit = 128}},
                       .activation_concurrency = {.limit = 128},
                       .state = state,
                       .security_identity = "test",
                       .owner_id = owner.owner_id,
                       .lease_generation = owner.lease_generation},
                     location_write_intent_t::new_claim).result ().value ().status);
    }
    location_options_t options;
    location_runtime_t runtime (store, options, "owner-query");
    store_location_runtime_query_t query (store, runtime, options);

    const auto topology = query.list_topology ({.mesh_name = "mesh-a"}).result ().value ();
    ASSERT_EQ (2u, topology.items.size ());
    EXPECT_TRUE (std::any_of (topology.items.begin (), topology.items.end (),
                              [] (const auto &entry) { return entry.draining; }));
    const auto summaries = query.list_service_summaries ({.mesh_name = "mesh-a"})
                             .result ().value ();
    ASSERT_EQ (1u, summaries.items.size ());
    EXPECT_EQ (2u, summaries.items.front ().total_count);
    EXPECT_EQ (2u, summaries.items.front ().ready_count);
}

TEST (ZLinkFrameworkStoreLocationResolvers, AutoConnectHostPublishesAndCleansLocalPeers)
{
    auto store = std::make_shared<in_memory_location_repository_t> ();
    location_options_t options;
    options.owner_lease_renew_interval = std::chrono::milliseconds (50);
    auto runtime = std::make_shared<location_runtime_t> (*store, options, "owner-auto");
    runtime->start (zlink::routing_id_t::from ("node-auto"));

    zlink::framework::zlink_builder_t zlink;
    auto orders = zlink.channel ("orders");
    orders.enable_server ()
      .set_routing_id (zlink::routing_id_t::from ("orders-router"))
      .peer_weight (zlink::peer_weight_t::value (7))
      .bind ("tcp://127.0.0.1:0");
    orders.enable_client ();
    auto events = zlink.channel ("events");
    events.enable_publisher ()
      .set_routing_id (zlink::routing_id_t::from ("events-pub"))
      .bind ("tcp://127.0.0.1:0");
    events.enable_subscriber ();

    zlink::framework::service_collection_t services;
    services.add_factory<zlink::framework::location_repository_t> (
      [store] (zlink::framework::service_provider_t &) {
          return std::static_pointer_cast<zlink::framework::location_repository_t> (store);
      },
      zlink::framework::service_lifetime_t::singleton);
    services.add_factory<zlink::framework::runtime::live_location_reader_t> (
      [store, options] (zlink::framework::service_provider_t &) {
          return std::make_shared<zlink::framework::runtime::live_location_reader_t> (*store,
                                                                                     options);
      },
      zlink::framework::service_lifetime_t::singleton);
    services.add_factory<location_runtime_t> (
      [runtime] (zlink::framework::service_provider_t &) { return runtime; },
      zlink::framework::service_lifetime_t::singleton);
    auto provider = services.build_provider ();

    zlink::framework::handler_registry_t handlers;
    zlink::framework::serializer_registry_t serializers;
    location_auto_connect_host_service_t service (
      zlink.message_bus (),
      zlink::framework::detail::channel_runtime_t::from (zlink.message_bus ())
        .channel_snapshots (),
      handlers, serializers);
    service.start (provider);

    const auto client_servers =
      store->list_client_servers ("orders").result ().value ();
    ASSERT_EQ (1u, client_servers.items.size ());
    EXPECT_EQ ("orders-router",
               client_servers.items.front ().server_rid.to_string ());
    EXPECT_TRUE (
      client_servers.items.front ().endpoint.starts_with (
        "tcp://127.0.0.1:"));
    EXPECT_NE ("tcp://127.0.0.1:0",
               client_servers.items.front ().endpoint);
    EXPECT_EQ (7, client_servers.items.front ().weight);
    const auto fanout_publishers =
      store->list_fanout_publishers ("events").result ().value ();
    ASSERT_EQ (1u, fanout_publishers.items.size ());
    EXPECT_EQ ("events",
               fanout_publishers.items.front ().channel_name);
    EXPECT_EQ ("events-pub",
               fanout_publishers.items.front ().publisher_rid.to_string ());
    EXPECT_TRUE (
      fanout_publishers.items.front ().endpoint.starts_with (
        "tcp://127.0.0.1:"));
    EXPECT_NE ("tcp://127.0.0.1:0",
               fanout_publishers.items.front ().endpoint);
    EXPECT_EQ (zlink::framework::framework_runtime_state_t::serving,
               fanout_publishers.items.front ().state);

    service.stop ();
    const auto remaining_client_servers =
      store->list_client_servers ("orders").result ().value ();
    EXPECT_TRUE (remaining_client_servers.items.empty ())
      << "remaining owner="
      << (remaining_client_servers.items.empty ()
            ? std::string ("<none>")
            : remaining_client_servers.items.front ().owner_id)
      << " generation="
      << (remaining_client_servers.items.empty ()
            ? 0
            : remaining_client_servers.items.front ().lease_generation);
    EXPECT_TRUE (
      store->list_fanout_publishers ("events").result ().value ().items.empty ());
    runtime->stop ();
}

TEST (ZLinkFrameworkStoreLocationResolvers,
      ClientServerRuntimeStateMappingMatchesServiceWire)
{
    using zlink::framework::framework_runtime_state_t;
    using zlink::framework::runtime::client_server::
      client_server_framework_state;
    using zlink::framework::runtime::client_server::
      client_server_service_state;
    using zlink::framework::runtime::mesh::service_node_state_t;

    const auto round_trip = [&] (framework_runtime_state_t state) {
        return client_server_framework_state (
          client_server_service_state (state));
    };

    EXPECT_EQ (framework_runtime_state_t::preparing,
               round_trip (framework_runtime_state_t::preparing));
    EXPECT_EQ (framework_runtime_state_t::serving,
               round_trip (framework_runtime_state_t::serving));
    EXPECT_EQ (framework_runtime_state_t::draining,
               round_trip (framework_runtime_state_t::relocating));
    EXPECT_EQ (framework_runtime_state_t::draining,
               round_trip (framework_runtime_state_t::relocated));
    EXPECT_EQ (framework_runtime_state_t::draining,
               round_trip (framework_runtime_state_t::draining));
    EXPECT_EQ (framework_runtime_state_t::stopped,
               round_trip (framework_runtime_state_t::stopped));
    EXPECT_EQ (framework_runtime_state_t::error,
               round_trip (framework_runtime_state_t::error));
    EXPECT_EQ (framework_runtime_state_t::relocating,
               client_server_framework_state (
                 service_node_state_t::retiring));
}

TEST (ZLinkFrameworkStoreLocationResolvers,
      AppClientServerDefaultConfigAdmissionUsesLocationAutoConnect)
{
    auto store = std::make_shared<in_memory_location_store_t> ();
    auto app = zlink::framework::app_t::create ();
    auto_connect_request_client_t *client = nullptr;
    const auto port = bindable_loopback_port (29700);

    app.add_zlink_framework ([&] (zlink::framework::zlink_framework_options_t &options) {
        options.add_location_store (store);
        options.handlers ()
          .group ("orders")
          .add<auto_connect_request_handler_t> ();
        options.add_client_server_channel ("orders")
          .server ()
          .set_bind_host ("127.0.0.1")
          .listen (port)
          .add_handler_group ("orders");
        options.add_client_server_channel ("orders").client ();
    });
    auto service = std::make_unique<auto_connect_request_client_t> (app);
    client = service.get ();
    app.add_hosted_service (std::move (service));

    EXPECT_EQ (0, app.run (0, nullptr));
    ASSERT_NE (nullptr, client);
    EXPECT_TRUE (client->observed) << client->last_error;
}

TEST (ZLinkFrameworkStoreLocationResolvers,
      ClientServerWithoutSelectableSnapshotReturnsTargetNotFound)
{
    auto store = std::make_shared<in_memory_location_store_t> ();
    auto app = zlink::framework::app_t::create ();
    missing_auto_connect_request_client_t *client = nullptr;

    app.add_zlink_framework ([&] (
                               zlink::framework::zlink_framework_options_t &options) {
        options.add_location_store (store);
        options.add_client_server_channel ("orders").client ();
    });
    auto service =
      std::make_unique<missing_auto_connect_request_client_t> (app);
    client = service.get ();
    app.add_hosted_service (std::move (service));

    EXPECT_EQ (0, app.run (0, nullptr));
    ASSERT_NE (nullptr, client);
    ASSERT_TRUE (client->observed_error.has_value ());
    EXPECT_EQ (
      zlink::framework::framework_error_kind_t::not_found,
      *client->observed_error);
}

TEST (ZLinkFrameworkStoreLocationResolvers,
      SameProcessClientServerUsesLocalReadyServerWithoutExternalStoreOrManualEndpoint)
{
    automatic_handler_scope_dependency_t::created.store (0);
    automatic_handler_scope_dependency_t::destroyed.store (0);
    automatic_handler_scope_filter_t::reject_requests.store (false);
    automatic_handler_scope_filter_t::last_filter_dependency.store (nullptr);
    automatic_scoped_request_handler_t::same_scope.store (false);
    automatic_scoped_request_handler_t::invocations.store (0);
    auto app = zlink::framework::app_t::create ();
    auto_connect_request_client_t *client = nullptr;

    app.add_zlink_framework ([&] (
                               zlink::framework::zlink_framework_options_t &options) {
        options.handlers ()
          .group ("orders")
          .add<automatic_scoped_request_handler_t> ();
        options.services ()
          .add_scoped<automatic_handler_scope_dependency_t> ();
        options.use_filter<automatic_handler_scope_filter_t> ();
        auto channel =
          options.add_client_server_channel ("orders");
        channel.server ()
          .set_bind_host ("127.0.0.1")
          .set_advertise_host ("127.0.0.1")
          .set_weight (7)
          .listen ()
          .add_handler_group ("orders");
        channel.client ();
    });
    auto service =
      std::make_unique<auto_connect_request_client_t> (app);
    client = service.get ();
    app.add_hosted_service (std::move (service));

    EXPECT_EQ (0, app.run (0, nullptr));
    ASSERT_NE (nullptr, client);
    EXPECT_TRUE (client->observed) << client->last_error;
    EXPECT_TRUE (automatic_scoped_request_handler_t::same_scope.load ());
    EXPECT_EQ (1, automatic_scoped_request_handler_t::invocations.load ());
    EXPECT_EQ (
      automatic_handler_scope_dependency_t::created.load (),
      automatic_handler_scope_dependency_t::destroyed.load ());
}

TEST (ZLinkFrameworkStoreLocationResolvers,
      SameProcessClientServerFilterRejectionReturnsTypedErrorAndDisposesScope)
{
    automatic_handler_scope_dependency_t::created.store (0);
    automatic_handler_scope_dependency_t::destroyed.store (0);
    automatic_handler_scope_filter_t::reject_requests.store (true);
    automatic_handler_scope_filter_t::last_filter_dependency.store (nullptr);
    automatic_scoped_request_handler_t::same_scope.store (false);
    automatic_scoped_request_handler_t::invocations.store (0);
    auto app = zlink::framework::app_t::create ();
    rejected_auto_connect_request_client_t *client = nullptr;

    app.add_zlink_framework ([&] (
                               zlink::framework::zlink_framework_options_t &options) {
        options.handlers ()
          .group ("orders")
          .add<automatic_scoped_request_handler_t> ();
        options.services ()
          .add_scoped<automatic_handler_scope_dependency_t> ();
        options.use_filter<automatic_handler_scope_filter_t> ();
        auto channel = options.add_client_server_channel ("orders");
        channel.server ()
          .set_bind_host ("127.0.0.1")
          .set_advertise_host ("127.0.0.1")
          .listen ()
          .add_handler_group ("orders");
        channel.client ();
    });
    auto service =
      std::make_unique<rejected_auto_connect_request_client_t> (app);
    client = service.get ();
    app.add_hosted_service (std::move (service));

    EXPECT_EQ (0, app.run (0, nullptr));
    ASSERT_NE (nullptr, client);
    EXPECT_TRUE (client->observed_rejected);
    EXPECT_EQ (0, automatic_scoped_request_handler_t::invocations.load ());
    EXPECT_GT (automatic_handler_scope_dependency_t::created.load (), 0);
    EXPECT_EQ (
      automatic_handler_scope_dependency_t::created.load (),
      automatic_handler_scope_dependency_t::destroyed.load ());
    automatic_handler_scope_filter_t::reject_requests.store (false);
}

TEST (ZLinkFrameworkStoreLocationResolvers,
      PublicSpotManagerUsesLocationReservationAndMeshCommands)
{
    auto app = zlink::framework::app_t::create ();
    user_spot_manager_client_t *client = nullptr;
    const auto endpoint =
      std::string ("tcp://127.0.0.1:")
      + std::to_string (bindable_loopback_port (29702));

    app.add_zlink_framework ([&] (
                               zlink::framework::zlink_framework_options_t &options) {
        auto node = options.add_route_mesh ("spot-mesh");
        node.set_routing_id (
              zlink::routing_id_t::from ("spot-local-node"))
          .listen (endpoint)
          .add_spot_factory<local_user_spot_t> (
            "room", [] (zlink::framework::spot_context_t context) {
                return std::make_shared<local_user_spot_t> (std::move (context));
            },
            [] (auto &factory) {
                factory.disable_relocation ();
            });
    });
    auto service =
      std::make_unique<user_spot_manager_client_t> (app);
    client = service.get ();
    app.add_hosted_service (std::move (service));

    EXPECT_EQ (0, app.run (0, nullptr));
    ASSERT_NE (nullptr, client);
    EXPECT_TRUE (client->observed) << client->last_error;
}

TEST (ZLinkFrameworkStoreLocationResolvers,
      ContextOnlySpotFactoryReceivesExactFrameworkIdentity)
{
    auto app = zlink::framework::app_t::create ();
    user_spot_manager_client_t *client = nullptr;
    const auto endpoint =
      std::string ("tcp://127.0.0.1:")
      + std::to_string (bindable_loopback_port (29705));
    std::string observed_mesh;
    std::string observed_node;
    std::string observed_spot;
    std::uint64_t observed_generation = 0;
    auto destruction_count = std::make_shared<std::atomic<int>> (0);

    app.add_zlink_framework ([&] (
                               zlink::framework::zlink_framework_options_t &options) {
        auto node = options.add_route_mesh ("context-mesh");
        node.set_routing_id (
              zlink::routing_id_t::from ("context-node"))
          .listen (endpoint)
          .add_spot_factory<context_owned_user_spot_t> (
            "room",
            [&] (zlink::framework::spot_context_t context) {
                observed_mesh = context.mesh_name ();
                observed_node = std::string (context.node_rid ().value ());
                observed_spot = context.spot_id ();
                observed_generation = context.object_generation ();
                return std::make_shared<context_owned_user_spot_t> (
                  std::move (context), destruction_count);
            },
            [] (auto &factory) {
                factory.disable_relocation ();
            });
    });
    auto service =
      std::make_unique<user_spot_manager_client_t> (app);
    client = service.get ();
    app.add_hosted_service (std::move (service));

    EXPECT_EQ (0, app.run (0, nullptr));
    ASSERT_NE (nullptr, client);
    EXPECT_TRUE (client->observed) << client->last_error;
    EXPECT_EQ ("context-mesh", observed_mesh);
    EXPECT_EQ ("context-node", observed_node);
    EXPECT_EQ ("local-user-spot", observed_spot);
    EXPECT_EQ (1u, observed_generation);
    EXPECT_EQ (1, destruction_count->load (std::memory_order_relaxed))
      << "host teardown must release the Spot instance/context ownership cycle";
}

TEST (ZLinkFrameworkStoreLocationResolvers,
      GeneratedUserSpotFirstAuthorityConflictFailsWithoutRetry)
{
    auto store = std::make_shared<test_location_repository_t> ();
    auto app = zlink::framework::app_t::create ();
    generated_user_spot_collision_client_t *client = nullptr;
    const auto endpoint =
      std::string ("tcp://127.0.0.1:")
      + std::to_string (bindable_loopback_port (29703));

    app.advanced ().services ().add_factory<
      zlink::framework::location_repository_t> (
      [store] (zlink::framework::service_provider_t &) {
          return std::static_pointer_cast<
            zlink::framework::location_repository_t> (
              store);
      },
      zlink::framework::service_lifetime_t::singleton);
    app.add_zlink_framework ([&] (
                               zlink::framework::zlink_framework_options_t &options) {
        auto node = options.add_route_mesh ("spot-collision-mesh");
        node.set_routing_id (
              zlink::routing_id_t::from (
                "spot-collision-node"))
          .listen (endpoint)
          .add_spot_factory<occupied_user_spot_t> (
            "occupied", [] (zlink::framework::spot_context_t context) {
                return std::make_shared<occupied_user_spot_t> (std::move (context));
            },
            [] (auto &factory) {
                factory.disable_relocation ();
            })
          .add_spot_factory<local_user_spot_t> (
            "room", [] (zlink::framework::spot_context_t context) {
                return std::make_shared<local_user_spot_t> (std::move (context));
            },
            [] (auto &factory) {
                factory.disable_relocation ();
            });
    });
    auto service =
      std::make_unique<
        generated_user_spot_collision_client_t> (
          app, store);
    client = service.get ();
    app.add_hosted_service (std::move (service));

    EXPECT_EQ (0, app.run (0, nullptr));
    ASSERT_NE (nullptr, client);
    EXPECT_TRUE (client->observed) << client->last_error;
}

TEST (ZLinkFrameworkStoreLocationResolvers,
      SourceCreatedReservationIsReconciledAfterExactCreateFailure)
{
    auto store = std::make_shared<test_location_repository_t> ();
    auto app = zlink::framework::app_t::create ();
    source_cleanup_client_t *client = nullptr;
    const auto endpoint =
      std::string ("tcp://127.0.0.1:")
      + std::to_string (bindable_loopback_port (29704));

    app.advanced ().services ().add_factory<
      zlink::framework::location_repository_t> (
      [store] (zlink::framework::service_provider_t &) {
          return std::static_pointer_cast<
            zlink::framework::location_repository_t> (
              store);
      },
      zlink::framework::service_lifetime_t::singleton);
    app.add_zlink_framework ([&] (
                               zlink::framework::zlink_framework_options_t &options) {
        auto node =
          options.add_route_mesh ("source-cleanup-mesh");
        node.set_routing_id (
              zlink::routing_id_t::from (
                "source-cleanup-node"))
          .listen (endpoint)
          .add_spot_factory<failing_user_spot_t> (
            "failing", [] (zlink::framework::spot_context_t context) {
                return std::make_shared<failing_user_spot_t> (std::move (context));
            },
            [] (auto &factory) {
                factory.disable_relocation ();
            });
    });
    auto service =
      std::make_unique<source_cleanup_client_t> (app);
    client = service.get ();
    app.add_hosted_service (std::move (service));

    EXPECT_EQ (0, app.run (0, nullptr));
    ASSERT_NE (nullptr, client);
    EXPECT_TRUE (client->observed) << client->last_error;
    EXPECT_GE (
      store->abort_count.load (std::memory_order_relaxed),
      2u);
    const auto authority =
      store
        ->read_authority ({"2:source-cleanup"})
        .result ()
        .value ();
    EXPECT_TRUE (
      std::holds_alternative<
        zlink::framework::authority_missing_t> (
        authority));
}

TEST (ZLinkFrameworkStoreLocationResolvers,
      UserSpotTerminalMappingPreservesExactPublicErrors)
{
    using zlink::framework::framework_error_kind_t;
    using zlink::framework::runtime::user_spot_terminal::
      map_user_spot_operation_failure;
    using zlink::framework::runtime::foundation::
      operation_terminal_t;
    using zlink::framework::runtime::protocol::
      reply_header_t;

    EXPECT_EQ (
      framework_error_kind_t::deadline_exceeded,
      map_user_spot_operation_failure (
        operation_terminal_t::timed_out, {}, true));
    EXPECT_EQ (
      framework_error_kind_t::invalid_operation,
      map_user_spot_operation_failure (
        operation_terminal_t::completed, {1, 107, 33},
        true));
    EXPECT_EQ (
      framework_error_kind_t::unavailable,
      map_user_spot_operation_failure (
        operation_terminal_t::completed, {1, 107, 34},
        false));
    EXPECT_EQ (
      framework_error_kind_t::type_mismatch,
      map_user_spot_operation_failure (
        operation_terminal_t::completed, {1, 107, 7},
        true));
    EXPECT_EQ (
      framework_error_kind_t::capacity_exceeded,
      map_user_spot_operation_failure (
        operation_terminal_t::completed, {1, 108, 0},
        true));
    EXPECT_EQ (
      framework_error_kind_t::rejected,
      map_user_spot_operation_failure (
        operation_terminal_t::completed, {1, 106, 15},
        true));
    EXPECT_EQ (
      framework_error_kind_t::invalid_operation,
      map_user_spot_operation_failure (
        operation_terminal_t::cancelled, {}, false));
    EXPECT_EQ (
      framework_error_kind_t::shutting_down,
      map_user_spot_operation_failure (
        operation_terminal_t::shutdown, {}, false));
}

TEST (ZLinkFrameworkStoreLocationResolvers,
      ClientServerPortZeroPublishesAdvertiseHostWithBoundPort)
{
    namespace client_server =
      zlink::framework::runtime::client_server;
    namespace protocol = zlink::framework::runtime::protocol;
    namespace mesh = zlink::framework::runtime::mesh;

    client_server::raw_client_server_server_t server (
      client_server::raw_client_server_server_options_t{
        protocol::client_server_server_admission_t{
          .channel_name = "orders",
          .server_routing_id = {'o', 'r', 'd', 'e', 'r', 's'},
          .lifecycle_generation = 1,
          .weight = 100,
          .state = mesh::service_node_state_t::preparing,
          .security_identity = "test",
          .effective_max_message_bytes = 1024,
          .advertised_endpoint = "tcp://127.0.0.1:*"},
        std::string ("service.example")});
    server.start ();
    const auto endpoint = server.endpoint ();
    EXPECT_TRUE (endpoint.starts_with (
      "tcp://service.example:"));
    EXPECT_EQ (std::string::npos, endpoint.find ('*'));
    EXPECT_GT (
      std::stoi (endpoint.substr (endpoint.rfind (':') + 1)),
      0);
    server.close ();
}

TEST (ZLinkFrameworkStoreLocationResolvers,
      ClientServerDuplicateRoleRegistrationFailsBeforeSocketBind)
{
    EXPECT_THROW (
      {
          auto app = zlink::framework::app_t::create ();
          app.add_zlink_framework (
            [] (zlink::framework::zlink_framework_options_t &options) {
                auto channel = options.add_client_server_channel ("orders");
                channel.client ().connect (
                  "tcp://127.0.0.1:29711");
                channel.client ().connect (
                  "tcp://127.0.0.1:29712");
            });
      },
      zlink::framework::framework_exception_t);

    EXPECT_THROW (
      {
          auto app = zlink::framework::app_t::create ();
          app.add_zlink_framework (
            [] (zlink::framework::zlink_framework_options_t &options) {
                options.handlers ()
                  .group ("orders")
                  .add<auto_connect_request_handler_t> ();
                auto channel = options.add_client_server_channel ("orders");
                channel.server ()
                  .set_bind_host ("127.0.0.1")
                  .listen (29713)
                  .add_handler_group ("orders");
                channel.server ()
                  .set_bind_host ("127.0.0.1")
                  .listen (29714);
            });
      },
      zlink::framework::framework_exception_t);
}

TEST (ZLinkFrameworkStoreLocationResolvers,
      ClientServerAllowsSameRoleOnDifferentChannelNames)
{
    EXPECT_NO_THROW (
      {
          auto app = zlink::framework::app_t::create ();
          app.add_zlink_framework (
            [] (zlink::framework::zlink_framework_options_t &options) {
                options.add_client_server_channel ("orders")
                  .client ()
                  .connect ("tcp://127.0.0.1:29721");
                options.add_client_server_channel ("billing")
                  .client ()
                  .connect ("tcp://127.0.0.1:29722");
            });
      });

    EXPECT_NO_THROW (
      {
          auto app = zlink::framework::app_t::create ();
          app.add_zlink_framework (
            [] (zlink::framework::zlink_framework_options_t &options) {
                options.handlers ()
                  .group ("orders")
                  .add<auto_connect_request_handler_t> ();
                options.add_client_server_channel ("orders")
                  .server ()
                  .set_bind_host ("127.0.0.1")
                  .listen (29723)
                  .add_handler_group ("orders");
                options.add_client_server_channel ("billing")
                  .server ()
                  .set_bind_host ("127.0.0.1")
                  .listen (29724)
                  .add_handler_group ("orders");
            });
      });
}

TEST (ZLinkFrameworkStoreLocationResolvers,
      RouteMeshAndClientServerChannelNameCollisionFailsInEitherBindOrder)
{
    EXPECT_THROW (
      {
          auto app = zlink::framework::app_t::create ();
          app.add_zlink_framework (
            [] (zlink::framework::zlink_framework_options_t &options) {
                options.add_route_mesh ("mesh-a")
                  .channel_name ("orders")
                  .client ();
                options.add_client_server_channel ("orders")
                  .client ()
                  .connect ("tcp://127.0.0.1:29715");
            });
      },
      zlink::framework::framework_exception_t);

    const auto mesh_send = [] (
                             zlink::framework::runtime::messaging::message_parts_t) {
        return zlink::framework::result_t<void>::success ();
    };
    const auto mesh_request = [] (
                                zlink::framework::runtime::messaging::message_parts_t parts,
                                std::chrono::milliseconds) {
        return zlink::framework::result_t<
          zlink::framework::runtime::messaging::message_parts_t>::success (
          std::move (parts));
    };
    const auto client_server_send = [] (
                                      std::string,
                                      std::string,
                                      zlink::message_t,
                                      std::chrono::milliseconds) {
        return zlink::framework::result_t<void>::success ();
    };
    const auto client_server_request = [] (
                                         std::string,
                                         std::string,
                                         zlink::message_t message,
                                         std::chrono::milliseconds) {
        return zlink::framework::result_t<zlink::message_t>::success (
          std::move (message));
    };

    zlink::framework::zlink_builder_t mesh_first_builder;
    auto mesh_first = zlink::framework::detail::channel_runtime_t::from (
      mesh_first_builder.message_bus ());
    mesh_first.bind_mesh_channel_transport ("orders", mesh_send, mesh_request);
    EXPECT_THROW (
      mesh_first.bind_client_server_transport (
        "orders", client_server_send, client_server_request),
      zlink::framework::framework_exception_t);

    zlink::framework::zlink_builder_t client_server_first_builder;
    auto client_server_first =
      zlink::framework::detail::channel_runtime_t::from (
        client_server_first_builder.message_bus ());
    client_server_first.bind_client_server_transport (
      "orders", client_server_send, client_server_request);
    EXPECT_THROW (
      client_server_first.bind_mesh_channel_transport (
        "orders", mesh_send, mesh_request),
      zlink::framework::framework_exception_t);
}

TEST (ZLinkFrameworkStoreLocationResolvers, AppFanoutPublishUsesLocationAutoConnect)
{
    auto store = std::make_shared<in_memory_location_store_t> ();
    auto app = zlink::framework::app_t::create ();
    auto_connect_publish_client_t *client = nullptr;
    auto_connect_event_handler_t::observed_count.store (0, std::memory_order_release);
    auto_connect_event_handler_t::observed_value.store (0, std::memory_order_release);
    auto_connect_event_handler_t::same_scope.store (false);
    blocked_auto_connect_event_handler_t::observed_count.store (0);
    automatic_handler_scope_dependency_t::created.store (0);
    automatic_handler_scope_dependency_t::destroyed.store (0);
    automatic_handler_scope_filter_t::reject_requests.store (false);
    automatic_handler_scope_filter_t::last_filter_dependency.store (nullptr);
    automatic_handler_scope_filter_t::blocked_fanout_dispatches.store (0);
    const auto endpoint =
      std::string ("tcp://127.0.0.1:") + std::to_string (bindable_loopback_port (29800));

    app.add_zlink_framework ([&] (zlink::framework::zlink_framework_options_t &options) {
        options.add_location_store (store);
        options.handlers ()
          .group ("events")
          .add_publish<auto_connect_event_handler_t> ()
          .add_publish<blocked_auto_connect_event_handler_t> ();
        options.services ()
          .add_scoped<automatic_handler_scope_dependency_t> ();
        options.use_filter<automatic_handler_scope_filter_t> ();
        options.add_fanout_channel ("events")
          .set_routing_id (zlink::routing_id_t::from ("events-publisher"))
          .enable_publisher (endpoint)
          .enable_subscriber ()
          .use_handler_group ("events");
    });
    auto service = std::make_unique<auto_connect_publish_client_t> (app);
    client = service.get ();
    app.add_hosted_service (std::move (service));

    EXPECT_EQ (0, app.run (0, nullptr));
    ASSERT_NE (nullptr, client);
    EXPECT_TRUE (client->observed) << client->last_error;
    EXPECT_GT (auto_connect_event_handler_t::observed_value.load (std::memory_order_acquire), 0);
    EXPECT_TRUE (auto_connect_event_handler_t::same_scope.load ());
    EXPECT_EQ (0, blocked_auto_connect_event_handler_t::observed_count.load ());
    EXPECT_GT (
      automatic_handler_scope_filter_t::blocked_fanout_dispatches.load (),
      0);
    EXPECT_EQ (
      automatic_handler_scope_dependency_t::created.load (),
      automatic_handler_scope_dependency_t::destroyed.load ());
}

TEST (ZLinkFrameworkStoreLocationResolvers, AutoConnectHostReconcilesRouteMeshConnections)
{
    auto store = std::make_shared<in_memory_location_repository_t> ();
    location_options_t options;
    options.owner_lease_renew_interval = std::chrono::milliseconds (50);
    auto runtime = std::make_shared<location_runtime_t> (*store, options, "owner-route-local");
    runtime->start (zlink::routing_id_t::from ("route-z-local-node"));

    seed_mesh_node (*store, "owner-route-remote", "route.mesh",
                    "route-a-remote-node", "inproc://route-remote");
    seed_mesh_node (*store, "owner-route-invalid-dealer", "route.mesh",
                    "route-dealer-peer", "inproc://route-invalid-dealer",
                    zlink::framework::framework_runtime_state_t::stopped);

    zlink::framework::zlink_builder_t zlink;
    zlink.route_channel ("route.mesh")
      .set_routing_id (zlink::routing_id_t::from ("route-z-local-node"))
      .connect ("inproc://route-manual");
    auto manager = zlink::framework::detail::channel_runtime_manager_t::from (zlink);
    manager.initialize_route_channels (zlink);
    auto &route = manager.get_route_channel ("route.mesh");

    zlink::framework::service_collection_t services;
    services.add_factory<zlink::framework::location_repository_t> (
      [store] (zlink::framework::service_provider_t &) {
          return std::static_pointer_cast<zlink::framework::location_repository_t> (store);
      },
      zlink::framework::service_lifetime_t::singleton);
    services.add_factory<zlink::framework::runtime::live_location_reader_t> (
      [store, options] (zlink::framework::service_provider_t &) {
          return std::make_shared<zlink::framework::runtime::live_location_reader_t> (*store,
                                                                                     options);
      },
      zlink::framework::service_lifetime_t::singleton);
    services.add_factory<location_runtime_t> (
      [runtime] (zlink::framework::service_provider_t &) { return runtime; },
      zlink::framework::service_lifetime_t::singleton);
    auto provider = services.build_provider ();

    zlink::framework::handler_registry_t handlers;
    zlink::framework::serializer_registry_t serializers;
    location_auto_connect_host_service_t service (
      zlink.message_bus (),
      zlink::framework::detail::channel_runtime_t::from (zlink.message_bus ())
        .channel_snapshots (),
      handlers, serializers);
    service.start (provider);

    EXPECT_TRUE (wait_until ([&] {
        const auto connections = route.list_connections ();
        return std::find (connections.begin (), connections.end (), "inproc://route-remote")
               != connections.end ();
    }));
    auto connected = route.list_connections ();
    EXPECT_NE (connected.end (),
               std::find (connected.begin (), connected.end (), "inproc://route-manual"));
    EXPECT_EQ (connected.end (),
               std::find (connected.begin (), connected.end (),
                          "inproc://route-invalid-dealer"));

    ASSERT_EQ (
      1,
      store
        ->remove_all_by_owner (
          live_owner_token (
            *store, "owner-route-remote"))
        .result ()
        .value ());
    EXPECT_TRUE (wait_until ([&] {
        const auto connections = route.list_connections ();
        return std::find (connections.begin (), connections.end (), "inproc://route-remote")
                 == connections.end ()
               && std::find (connections.begin (), connections.end (), "inproc://route-manual")
                    != connections.end ();
    }));

    service.stop ();
    ASSERT_EQ (1,
               store
                 ->remove_all_by_owner (
                   live_owner_token (
                     *store,
                     "owner-route-invalid-dealer"))
                 .result ()
                 .value ());
    runtime->stop ();
}

TEST (ZLinkFrameworkStoreLocationResolvers, AutoConnectHostUsesRouteMeshInitiatorOrdering)
{
    auto store = std::make_shared<in_memory_location_repository_t> ();
    location_options_t options;
    options.owner_lease_renew_interval = std::chrono::milliseconds (50);

    auto lower_runtime =
      std::make_shared<location_runtime_t> (*store, options, "owner-route-lower-local");
    lower_runtime->start (zlink::routing_id_t::from ("route-a-local-node"));
    seed_mesh_node (*store, "owner-route-higher-remote", "route.lower",
                    "route-z-remote-node", "inproc://route-higher-remote");

    zlink::framework::zlink_builder_t lower_zlink;
    lower_zlink.route_channel ("route.lower")
      .bind ("inproc://route-lower-local")
      .set_routing_id (zlink::routing_id_t::from ("route-a-local-node"));
    auto lower_manager =
      zlink::framework::detail::channel_runtime_manager_t::from (lower_zlink);
    lower_manager.initialize_route_channels (lower_zlink);
    auto &lower_route = lower_manager.get_route_channel ("route.lower");

    zlink::framework::service_collection_t lower_services;
    lower_services.add_factory<zlink::framework::location_repository_t> (
      [store] (zlink::framework::service_provider_t &) {
          return std::static_pointer_cast<zlink::framework::location_repository_t> (store);
      },
      zlink::framework::service_lifetime_t::singleton);
    lower_services.add_factory<zlink::framework::runtime::live_location_reader_t> (
      [store, options] (zlink::framework::service_provider_t &) {
          return std::make_shared<zlink::framework::runtime::live_location_reader_t> (*store,
                                                                                     options);
      },
      zlink::framework::service_lifetime_t::singleton);
    lower_services.add_factory<location_runtime_t> (
      [lower_runtime] (zlink::framework::service_provider_t &) { return lower_runtime; },
      zlink::framework::service_lifetime_t::singleton);
    auto lower_provider = lower_services.build_provider ();

    zlink::framework::handler_registry_t lower_handlers;
    zlink::framework::serializer_registry_t lower_serializers;
    location_auto_connect_host_service_t lower_service (lower_zlink.message_bus (),
                                                        zlink::framework::detail::channel_runtime_t::from (
                                                          lower_zlink.message_bus ())
                                                          .channel_snapshots (),
                                                        lower_handlers,
                                                        lower_serializers);
    lower_service.start (lower_provider);

    EXPECT_TRUE (wait_until ([&] {
        const auto connections = lower_route.list_connections ();
        const auto targets = lower_route.list_connection_targets ();
        return std::find (connections.begin (), connections.end (), "inproc://route-higher-remote")
                 != connections.end ()
               && std::any_of (targets.begin (), targets.end (), [] (const auto &target) {
                      return target.endpoint == "inproc://route-higher-remote" && target.peer_rid
                             && target.peer_rid->to_string () == "route-z-remote-node";
                  });
    }));
    lower_service.stop ();
    lower_runtime->stop ();

    ASSERT_EQ (
      1,
      store
        ->remove_all_by_owner (
          live_owner_token (
            *store,
            "owner-route-higher-remote"))
        .result ()
        .value ());

    auto higher_runtime =
      std::make_shared<location_runtime_t> (*store, options, "owner-route-higher-local");
    higher_runtime->start (zlink::routing_id_t::from ("route-z-local-node"));
    seed_mesh_node (*store, "owner-route-lower-remote", "route.higher",
                    "route-a-remote-node", "inproc://route-lower-remote");

    zlink::framework::zlink_builder_t higher_zlink;
    higher_zlink.route_channel ("route.higher")
      .bind ("inproc://route-higher-local")
      .set_routing_id (zlink::routing_id_t::from ("route-z-local-node"));
    auto higher_manager =
      zlink::framework::detail::channel_runtime_manager_t::from (higher_zlink);
    higher_manager.initialize_route_channels (higher_zlink);
    auto &higher_route = higher_manager.get_route_channel ("route.higher");

    zlink::framework::service_collection_t higher_services;
    higher_services.add_factory<zlink::framework::location_repository_t> (
      [store] (zlink::framework::service_provider_t &) {
          return std::static_pointer_cast<zlink::framework::location_repository_t> (store);
      },
      zlink::framework::service_lifetime_t::singleton);
    higher_services.add_factory<zlink::framework::runtime::live_location_reader_t> (
      [store, options] (zlink::framework::service_provider_t &) {
          return std::make_shared<zlink::framework::runtime::live_location_reader_t> (*store,
                                                                                     options);
      },
      zlink::framework::service_lifetime_t::singleton);
    higher_services.add_factory<location_runtime_t> (
      [higher_runtime] (zlink::framework::service_provider_t &) { return higher_runtime; },
      zlink::framework::service_lifetime_t::singleton);
    auto higher_provider = higher_services.build_provider ();

    zlink::framework::handler_registry_t higher_handlers;
    zlink::framework::serializer_registry_t higher_serializers;
    location_auto_connect_host_service_t higher_service (higher_zlink.message_bus (),
                                                         zlink::framework::detail::channel_runtime_t::from (
                                                           higher_zlink.message_bus ())
                                                           .channel_snapshots (),
                                                         higher_handlers,
                                                         higher_serializers);
    higher_service.start (higher_provider);

    std::this_thread::sleep_for (std::chrono::milliseconds (250));
    const auto higher_connections = higher_route.list_connections ();
    EXPECT_EQ (higher_connections.end (),
               std::find (higher_connections.begin (), higher_connections.end (),
                          "inproc://route-lower-remote"));

    higher_service.stop ();
    higher_runtime->stop ();
    ASSERT_EQ (
      1,
      store
        ->remove_all_by_owner (
          live_owner_token (
            *store,
            "owner-route-lower-remote"))
        .result ()
        .value ());
}

TEST (ZLinkFrameworkStoreLocationResolvers, AppStreamHostStartsAndStopsTcpListener)
{
    auto app = zlink::framework::app_t::create ();
    const auto endpoint =
      std::string ("tcp://127.0.0.1:") + std::to_string (bindable_loopback_port (29600));

    app.add_zlink_framework ([&] (zlink::framework::zlink_framework_options_t &options) {
        options.add_stream_node ("stream-node")
          .bind (endpoint)
          .register_session<echo_stream_session_t> ();
    });
    auto client = std::make_unique<stream_roundtrip_client_t> (
      app, endpoint,
      zlink::framework::detail::stream_runtime_t::from (app.advanced ().zlink ()));
    auto *client_ptr = client.get ();
    app.add_hosted_service (std::move (client));

    EXPECT_EQ (0, app.run (0, nullptr));
    ASSERT_NE (nullptr, client_ptr);
    EXPECT_TRUE (client_ptr->observed) << client_ptr->last_error_message ();
}

} // namespace
