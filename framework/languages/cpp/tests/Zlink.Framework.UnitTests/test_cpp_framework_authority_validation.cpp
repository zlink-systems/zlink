/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include "runtime/stateful/public_host_runtime.hpp"
#include "runtime/stateful/raw_stateful_dispatch.hpp"
#include "runtime/messaging/request_failure_mapper.hpp"
#include "runtime/backend/raw_binding_adapter.hpp"
#include "runtime_failure_fixture.hpp"

#include <iostream>
#include <stdexcept>

namespace
{
using namespace zlink::framework;
namespace st = runtime::stateful;
namespace mesh = runtime::mesh;
namespace protocol = runtime::protocol;

void require (bool value, const char *message)
{
    if (!value)
        throw std::runtime_error (message);
}

void verify ()
{
    runtime::host::public_host_runtime_t host (runtime::host::host_options_t{
      test::runtime_failure_options (mesh::raw_mesh_node_options_t{
        .descriptor = {"validation", {'t'}, 1, 1, "tcp://127.0.0.1:0", {},
                       mesh::service_node_state_t::preparing}})});
    std::size_t lookups = 0;
    bool unavailable = false;
    std::uint64_t lease = 1;
    st::raw_stateful_dispatch_t dispatch (
      host.objects (), host.transport (),
      [&] (const auto &) -> task_t<std::optional<st::accepted_record_authority_t>> {
          ++lookups;
          if (unavailable)
              throw framework_exception_t (framework_error_kind_t::unavailable,
                                           "authority unavailable");
          co_return st::accepted_record_authority_t{{"owner", 1, {'s'}, 1}, lease};
      });
    const st::object_ref_t actor{st::object_kind_t::actor, "actor", 1, 1, "validation", "t"};
    protocol::actor_message_header_t header;
    header.target = {"actor", 1, {'t'}, 1, 1, 1};
    const auto resolve = [&] {
        return dispatch.accepted_actor_authority (actor, header, {'s'}, 1, {}, false, {})
          .result ().value ();
    };
    require (resolve ().first == st::stateful_error_t::none && lookups == 1,
             "normal authority was rejected");
    header.target.actor_id = "other";
    require (resolve ().first == st::stateful_error_t::conflict && lookups == 1,
             "route conflict reached authority resolver");
    header.target.actor_id = "actor";
    lease = 2;
    require (resolve ().first == st::stateful_error_t::conflict,
             "authority lease fence conflict changed");
    lease = 1;
    unavailable = true;
    require (resolve ().first == st::stateful_error_t::unavailable,
             "typed Unavailable became an unexpected failure");
    const auto before_malformed = lookups;
    mesh::service_mailbox_record_t malformed;
    malformed.domain = mesh::service_mailbox_domain_t::application;
    malformed.source_routing_id = {'s'};
    malformed.source_node_generation = 1;
    malformed.parts = detail::backend::materialize_binding_parts ({{0}, {0}});
    require (dispatch.ingest_claimed (actor, std::move (malformed), {}).result ().value ()
               == st::stateful_error_t::invalid && lookups == before_malformed,
             "malformed wire escaped decoder validation or reached authority lookup");
    const auto failure = runtime::messaging::request_failure_mapper_t{}.target_failure_reply (
      framework_error_kind_t::protocol_error);
    require (failure && failure->terminal_result
               == static_cast<std::uint32_t> (protocol::request_terminal_result::protocolError)
               && failure->failure_code
               == static_cast<std::uint32_t> (protocol::framework_error_code::requestProtocolError),
             "ProtocolError failure mapping changed");
    host.close ();
}
}
int main ()
{
    try { verify (); return 0; }
    catch (const std::exception &error) {
        std::cerr << error.what () << std::endl;
        return 1;
    }
}
