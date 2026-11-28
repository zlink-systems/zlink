/* SPDX-License-Identifier: Elastic-2.0 */
#pragma once

#include "runtime/locations/location_repository.hpp"
#include "runtime/actors/actor_ref_access.hpp"

#include <service_wire_codec.hpp>

#include <zlink/framework/contracts/messaging/message.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace zlink::framework::runtime::mesh_node_host_service_detail
{

namespace terminal_codec = zlink::framework::runtime::protocol::generated::detail;

actor_create_result_t actor_result_from_terminal (const creation_terminal_record_t &terminal,
                                                  const node_rid_t &target_node_rid,
                                                  const std::string &stable_type,
                                                  auto &&wrap_message)
{
    std::vector<std::uint8_t> bytes;
    bytes.reserve (terminal.terminal_envelope.size ());
    for (const auto byte : terminal.terminal_envelope)
        bytes.push_back (std::to_integer<std::uint8_t> (byte));
    const terminal_codec::codec_context_t context{1024 * 1024, 1024 * 1024};
    terminal_codec::reader_t reader{bytes};
    terminal_codec::creation_operation_terminal_v1_t decoded;
    if (terminal_codec::decode_value_creation_operation_terminal_v1 (reader, decoded, context)
          != terminal_codec::error_code::ok
        || !reader.empty ())
        throw std::invalid_argument ("creation terminal schema decoding failed");
    if (decoded.terminalResult != terminal_codec::request_terminal_result_t::ok)
        throw framework_exception_t (framework_error_kind_t::internal_failure,
                                     "Actor creation operation previously failed");
    if (!decoded.creation)
        throw std::invalid_argument ("creation terminal schema has no creation result");
    std::optional<message_t> reply;
    if (decoded.applicationPayload) {
        reply = wrap_message (zlink::message_t::from (decoded.applicationPayload->payload.value));
    }
    const auto &creation = *decoded.creation;
    if (creation.createResult == terminal_codec::actor_create_result_t::existing) {
        const auto &actor =
          std::get<terminal_codec::actor_create_terminal_t::case_0_t> (creation.value).actor;
        return actor_create_existing_t{::zlink::framework::detail::actor_ref_access_t::make (
          target_node_rid, stable_type, actor.actorId.value, actor.objectGeneration.value)};
    }
    if (creation.createResult == terminal_codec::actor_create_result_t::created) {
        const auto &actor =
          std::get<terminal_codec::actor_create_terminal_t::case_1_t> (creation.value).actor;
        return actor_create_created_t{
          ::zlink::framework::detail::actor_ref_access_t::make (
            target_node_rid, stable_type, actor.actorId.value, actor.objectGeneration.value),
          std::move (reply)};
    }
    if (creation.createResult == terminal_codec::actor_create_result_t::rejected)
        return actor_create_rejected_t{std::move (reply)};
    throw std::invalid_argument ("creation terminal schema has an invalid creation result");
}

} // namespace zlink::framework::runtime::mesh_node_host_service_detail
