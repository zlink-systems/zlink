/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include "runtime/mesh/raw_mesh_node_owner.hpp"
#include "runtime/mesh/receive_path_probe.hpp"
#include "../support/native_call_counter.hpp"

#include <gtest/gtest.h>
#include <zlink.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <future>
#include <optional>
#include <string_view>
#include <thread>

namespace mesh = zlink::framework::runtime::mesh;
namespace wire = zlink::framework::runtime::protocol;
namespace backend = zlink::framework::detail::backend;
using namespace std::chrono_literals;

namespace zlink::framework::runtime::mesh
{
struct receive_record_test_access_t
{
    static auto lease (raw_mesh_node_owner_t &node) { return node.lease_receive_port (); }
};
}

namespace
{
struct native_original_t
{
    const void *body = nullptr;
    std::size_t size = 0;
    std::optional<zlink::routing_id_t> rid;
    std::optional<zlink::reply_token_t> token;
    std::uint64_t generation = 0;
};
struct observation_t
{
    mesh::raw_mesh_node_owner_t *node;
    std::array<native_original_t, 3> originals;
    std::size_t records = 0;
    std::size_t body_copies = 0;
    std::string_view fault;
};
thread_local observation_t *body_watch = nullptr;
}

namespace
{
bool exclude_native_body (const void *data, std::size_t size) noexcept
{
    if (body_watch)
        for (std::size_t index = 0; index < body_watch->records; ++index)
            if (data == body_watch->originals[index].body
                && size == body_watch->originals[index].size)
                return false;
    return true;
}

void observe_body_copy (const zlink::message_t &message)
{
    if (body_watch)
        for (std::size_t index = 0; index < body_watch->records; ++index)
            if (message.data () == body_watch->originals[index].body
                && message.size () == body_watch->originals[index].size) {
                ++body_watch->body_copies;
                break;
            }
}
}

// Observe public binding body materialization, including a discarded extra clone.
// Native message handle copies share the body and are not charged here.
extern "C" std::vector<std::uint8_t>
__real__ZNK5zlink9message_t8to_bytesEv (const zlink::message_t *message);
extern "C" std::vector<std::uint8_t>
__wrap__ZNK5zlink9message_t8to_bytesEv (const zlink::message_t *message)
{
    observe_body_copy (*message);
    return __real__ZNK5zlink9message_t8to_bytesEv (message);
}
extern "C" zlink::message_t __real__ZNK5zlink9message_t5cloneEv (const zlink::message_t *message);
extern "C" zlink::message_t __wrap__ZNK5zlink9message_t5cloneEv (const zlink::message_t *message)
{
    observe_body_copy (*message);
    return __real__ZNK5zlink9message_t5cloneEv (message);
}

namespace
{
std::string_view fault ()
{
    const auto value = std::getenv ("ZLINK_RECEIVE_RECORD_NEGATIVE");
    return value ? std::string_view (value) : std::string_view{};
}

class probe_scope_t
{
  public:
    explicit probe_scope_t (observation_t &observation) : _probe{.context = &observation}
    {
        _probe.received = [] (void *context, mesh::service_mailbox_record_t &record) {
            auto &observation = *static_cast<observation_t *> (context);
            if (observation.records >= observation.originals.size ()
                || record.parts ().size () != 2)
                return;
            const auto &body = record.parts ().back ();
            observation.originals[observation.records++] = {
              body.data (), body.size (), record.received.routing_id (), record.reply_token (),
              record.received.route_generation ()};
            if (observation.records != 1)
                return;
            if (observation.fault == "copy") {
                const auto extra = body.copy ();
                EXPECT_EQ (extra.size (), body.size ());
            } else if (observation.fault == "clone") {
                const auto extra = body.clone ();
                EXPECT_EQ (extra.size (), body.size ());
            } else if (observation.fault == "classification") {
                (void) record.service_header ();
                record._service_header.reset ();
            } else if (observation.fault == "lease") {
                (void) mesh::receive_record_test_access_t::lease (*observation.node);
            }
        };
        body_watch = &observation;
        mesh::receive_path_probe = &_probe;
    }
    ~probe_scope_t () { stop (); }
    void stop ()
    {
        mesh::receive_path_probe = nullptr;
        body_watch = nullptr;
    }
    const mesh::receive_path_probe_t &counts () const { return _probe; }

  private:
    mesh::receive_path_probe_t _probe;
};

mesh::raw_mesh_node_options_t options (std::string_view name, std::uint64_t generation)
{
    mesh::raw_mesh_node_options_t result;
    result.runtime_failures =
      std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ();
    result.descriptor = {.mesh_name = "receive-record",
                         .node_routing_id = {name.begin (), name.end ()},
                         .lifecycle_generation = generation,
                         .descriptor_revision = 1,
                         .advertised_endpoint = "inproc://" + std::string (name)};
    return result;
}

class ReceiveRecord : public testing::Test
{
  protected:
    zlink::framework::completion_owner_t completion_owner;
    std::shared_ptr<zlink::context_t> context = std::make_shared<zlink::context_t> ();
    mesh::raw_mesh_node_owner_t source{options ("receive-source", 101), context};
    mesh::raw_mesh_node_owner_t target{options ("receive-target", 201), context};
    std::atomic_size_t completed = 0;
    std::atomic_size_t failed = 0;

    static auto now () { return mesh::service_liveness_registry_t::clock_t::now (); }
    void progress (mesh::raw_mesh_node_owner_t &node)
    {
        (void) node.observe_routes ();
        (void) node.pump_one (now ()).result ().value ();
    }
    void SetUp () override
    {
        source.start ();
        target.start ();
        ASSERT_TRUE (
          source.connect_peer (target.endpoint (), target.topology ().local_descriptor ()));
        const auto deadline = now () + 3s;
        const auto admitted = [&] {
            const auto first =
              source.topology ().peer (target.startup_descriptor ().node_routing_id);
            const auto second =
              target.topology ().peer (source.startup_descriptor ().node_routing_id);
            return first && first->ready && second && second->ready;
        };
        while (!admitted () && now () < deadline) {
            progress (source);
            progress (target);
        }
        ASSERT_TRUE (admitted ());
        // Complete the existing handshake before observing application ingress.
        for (int index = 0; index != 4; ++index) {
            progress (source);
            progress (target);
        }
    }
    void TearDown () override
    {
        target.close ();
        source.close ();
    }
    wire::application_payload_t payload ()
    {
        return {"receive.probe", "application/octet-stream", std::vector<std::uint8_t> (4096, 42)};
    }
    void request ()
    {
        auto caller = source.request_to_node (
          target.startup_descriptor ().node_routing_id, payload (), 3s,
          [this] (auto terminal, auto) {
              if (terminal
                  == zlink::framework::runtime::foundation::operation_terminal_t::completed)
                  completed.fetch_add (1, std::memory_order_relaxed);
              else
                  failed.fetch_add (1, std::memory_order_relaxed);
          });
        ASSERT_TRUE (caller.result ().value ());
        // The caller's wrapper is destroyed while its original is still queued.
    }
    auto take ()
    {
        auto claim =
          target.mailbox ().try_claim (mesh::service_mailbox_domain_t::application, 3, 65536);
        if (!claim)
            throw std::runtime_error ("receive fixture has no queued record");
        return claim;
    }
};

TEST_F (ReceiveRecord, CountsAndOriginalTokenAcrossOneRotation)
{
    for (int index = 0; index != 3; ++index)
        request ();
    observation_t observation{.node = &target, .fault = fault ()};
    probe_scope_t probe (observation);
    zlink::framework::tests::native_call_scope_t native (exclude_native_body);
    {
        const auto turn = target.begin_receive_turn ();
        for (int index = 0; index != 3; ++index)
            ASSERT_EQ (target.receive_next (turn, now ()),
                       mesh::raw_mesh_pump_result_t::application);
        EXPECT_EQ (target.receive_next (turn, now ()), mesh::raw_mesh_pump_result_t::no_data);
    }
    ASSERT_EQ (observation.records, 3u);
    auto claim = take ();
    ASSERT_EQ (claim->claimed_messages, 3u);
    std::array<std::optional<mesh::service_mailbox_record_t>, 3> owned_records;
    for (std::size_t index = 0; index != 3; ++index) {
        owned_records[index] = target.mailbox ().take ({claim, index});
        auto &record = owned_records[index];
        ASSERT_TRUE (record);
        const auto &original = observation.originals[index];
        ASSERT_EQ (record->parts ().size (), 2u);
        EXPECT_EQ (record->parts ().back ().data (), original.body);
        EXPECT_EQ (record->parts ().back ().size (), original.size);
        EXPECT_EQ (record->received.routing_id (), original.rid);
        EXPECT_EQ (record->received.route_generation (), original.generation);
        EXPECT_EQ (record->reply_token (), observation.fault == "token" && index == 0
                                             ? observation.originals[1].token
                                             : original.token);
        EXPECT_EQ (record->service_header ().kind, wire::command::nodeRequest);
        EXPECT_EQ (record->application_kind (),
                   zlink::framework::runtime::host::record_kind_t::node_request);
        EXPECT_EQ (record->owner, "node");
        EXPECT_TRUE (target.reply (*record, payload ()));
    }
    EXPECT_NE (observation.originals[0].token, observation.originals[1].token);
    (void) target.mailbox ().release (*claim);
    EXPECT_EQ (observation.body_copies, 0u);
    EXPECT_EQ (native[6], 0u);
    EXPECT_EQ (probe.counts ().classifications, 3u);
    EXPECT_EQ (probe.counts ().port_leases, 1u);
    std::printf ("receive records=%zu body_copies=%zu native_body_copies=%zu classifications=%zu "
                 "port_leases=%zu fault=%.*s\n",
                 observation.records, observation.body_copies, native[6],
                 probe.counts ().classifications, probe.counts ().port_leases,
                 static_cast<int> (observation.fault.size ()),
                 observation.fault.empty () ? "" : observation.fault.data ());
    probe.stop ();
    const auto deadline = now () + 3s;
    while (completed.load (std::memory_order_relaxed) != 3 && now () < deadline)
        progress (source);
    EXPECT_EQ (completed.load (std::memory_order_relaxed), 3u);
    EXPECT_EQ (failed.load (std::memory_order_relaxed), 0u);
}

TEST_F (ReceiveRecord, CallerWrapperDestructionKeepsQueuedNativeStorage)
{
    request ();
    const auto turn = target.begin_receive_turn ();
    const void *original_body = nullptr;
    std::optional<zlink::reply_token_t> original_token;
    {
        zlink::received_t caller_wrapper;
        ASSERT_TRUE (
          turn.port->receive_if_ready (caller_wrapper, zlink::poll_event_flag_t::pollin));
        original_body = caller_wrapper.parts ().back ().data ();
        original_token = caller_wrapper.reply_token ();
        auto record = mesh::service_mailbox_record_t::from_received (
          "node", mesh::service_mailbox_domain_t::application, std::move (caller_wrapper));
        record.correlation =
          wire::decode_node_request_header (wire::message_bytes (record.parts ().front ()));
        ASSERT_TRUE (target.mailbox ().try_enqueue (std::move (record)));
    }
    auto claim = take ();
    if (fault () == "drop")
        target.mailbox ().prepare ({claim, 0}, [] (auto &record) {
            record.received.close ();
            return true;
        });
    auto record = target.mailbox ().take ({claim, 0});
    ASSERT_TRUE (record);
    ASSERT_EQ (record->parts ().size (), 2u);
    EXPECT_EQ (record->parts ().back ().data (), original_body);
    EXPECT_EQ (record->reply_token (), original_token);
    EXPECT_TRUE (target.reply (*record, payload ()));
    (void) target.mailbox ().release (*claim);
}

TEST_F (ReceiveRecord, CloseDuringRotationKeepsLeaseAndRejectsLaterReply)
{
    request ();
    const auto turn = target.begin_receive_turn ();
    ASSERT_EQ (target.receive_next (turn, now ()), mesh::raw_mesh_pump_result_t::application);
    auto claim = take ();
    auto record = target.mailbox ().take ({claim, 0});
    ASSERT_TRUE (record);
    const auto token = record->reply_token ();
    const auto generation = record->received.route_generation ();
    std::promise<void> entered;
    auto entered_future = entered.get_future ();
    auto poll = std::async (std::launch::async, [port = turn.port, &entered] {
        entered.set_value ();
        return port->poll (2s);
    });
    entered_future.get ();
    std::jthread closer ([&] { target.close (); });
    closer.join ();
    ASSERT_EQ (poll.wait_for (2s), std::future_status::ready);
    EXPECT_NO_THROW ((void) poll.get ());
    EXPECT_EQ (target.receive_next (turn, now ()), mesh::raw_mesh_pump_result_t::no_data);
    EXPECT_EQ (record->reply_token (), token);
    EXPECT_EQ (record->received.route_generation (), generation);
    EXPECT_FALSE (target.reply (*record, payload ()));
    EXPECT_EQ (record->parts ().size (), 2u);
    (void) target.mailbox ().release (*claim);
}

TEST_F (ReceiveRecord, MalformedPrefixTruncationAndPartCountsKeepProtocolError)
{
    const auto good_header = wire::encode_node_request_header (17);
    const auto body = wire::encode_application_payload (payload ());
    std::array<backend::raw_message_t, 5> bad{
      backend::raw_message_t{{0, 0, 1, static_cast<std::uint8_t> (wire::command::nodeRequest), 0},
                             body},
      backend::raw_message_t{{'Z', 'M'}, body},
      backend::raw_message_t{
        std::vector<std::uint8_t> (good_header.begin (), good_header.end () - 1), body},
      backend::raw_message_t{good_header}, backend::raw_message_t{good_header, body, body}};
    const auto port = mesh::receive_record_test_access_t::lease (source);
    std::vector<zlink::framework::task_t<backend::raw_request_completion_t>> pending;
    for (const auto &parts : bad) {
        pending.push_back (port->request (
          target.startup_descriptor ().node_routing_id,
          fault () == "malformed" ? backend::raw_message_t{good_header, body} : parts, 3s));
        const auto turn = target.begin_receive_turn ();
        EXPECT_EQ (target.receive_next (turn, now ()),
                   mesh::raw_mesh_pump_result_t::protocol_error);
        EXPECT_EQ (target.mailbox ().pending_messages (mesh::service_mailbox_domain_t::application),
                   0u);
    }
    // A recoverable request with the wrong count still gets the same wire terminal.
    auto &request = pending.back ();
    const auto deadline = now () + 3s;
    while (!request.await_ready () && now () < deadline)
        (void) port->poll (10ms);
    ASSERT_TRUE (request.await_ready ());
    const auto &reply = request.result ().value ();
    ASSERT_EQ (reply.terminal, zlink::request_result_t::ok);
    ASSERT_EQ (reply.parts.size (), 1u);
    const auto terminal = wire::decode_reply_header (wire::message_bytes (reply.parts.front ()));
    EXPECT_EQ (terminal.correlation, 17u);
    EXPECT_EQ (terminal.terminal_result,
               static_cast<std::uint32_t> (wire::request_terminal_result::protocolError));
    EXPECT_EQ (terminal.failure_code,
               static_cast<std::uint32_t> (wire::framework_error_code::requestProtocolError));
    // The same complete shape remains accepted.
    ASSERT_TRUE (source.send_to_node (target.startup_descriptor ().node_routing_id, payload ())
                   .result ()
                   .value ());
    const auto valid_turn = target.begin_receive_turn ();
    EXPECT_EQ (target.receive_next (valid_turn, now ()), mesh::raw_mesh_pump_result_t::application);
}
}
