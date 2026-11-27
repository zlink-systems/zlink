/* SPDX-License-Identifier: Elastic-2.0 */
#include "runtime/streams/admission_queue_messages.hpp"
#include <zlink/framework/contracts/errors/result.hpp>

#include <charconv>
#include <limits>

namespace zlink::framework::runtime::admission_queue
{
namespace
{

[[noreturn]] void invalid_message ()
{
    throw framework_exception_t (framework_error_kind_t::protocol_error,
                                 "Invalid STREAM admission queue message");
}

const json_t &required (const json_t &object, const char *key)
{
    if (const auto field = object.find (key); field != object.end ())
        return *field;
    invalid_message ();
}

const std::string &text (const json_t &value)
{
    const auto *string = value.get_ptr<const std::string *> ();
    if (!string)
        invalid_message ();
    return *string;
}

std::uint64_t decimal (const json_t &field, bool zero_allowed = false)
{
    const auto &value = text (field);
    std::uint64_t result = 0;
    const auto end = value.data () + value.size ();
    const auto parsed = std::from_chars (value.data (), end, result);
    if (parsed.ec != std::errc{} || parsed.ptr != end
        || (value.size () > 1 && value.front () == '0') || (!zero_allowed && result == 0))
        invalid_message ();
    return result;
}

std::int32_t integer (const json_t &value, std::int32_t min)
{
    if (!value.is_number_integer () || value < min
        || value > std::numeric_limits<std::int32_t>::max ())
        invalid_message ();
    return value.get<std::int32_t> ();
}

bytes_t rid (const json_t &value)
{
    const auto &encoded = text (value);
    if (encoded.empty () || encoded.size () > 340)
        invalid_message ();
    auto bytes = decode_base64url (encoded);
    if (!bytes)
        invalid_message ();
    return std::move (*bytes);
}

template <typename Fn> auto list (const json_t &values, Fn read)
{
    if (!values.is_array ())
        invalid_message ();
    std::vector<decltype (read (values[0]))> result;
    result.reserve (values.size ());
    for (const auto &value : values)
        result.push_back (read (value));
    return result;
}

node_t read_node (const json_t &value)
{
    return {rid (required (value, "nodeRid")),
            decimal (required (value, "nodeLifecycleGeneration"))};
}

session_t read_session (const json_t &value)
{
    const auto &stream = text (required (value, "streamName"));
    if (!valid_stream_name (stream))
        invalid_message ();
    return {stream, rid (required (value, "sessionRid"))};
}

connection_t read_connection (const json_t &value)
{
    return connection_of (read_node (value), read_session (value));
}

delivery_t read_delivery (const json_t &value)
{
    auto session = read_session (value);
    const auto &kind = text (required (value, "kind"));
    if (kind != "position" && kind != "ticket")
        invalid_message ();
    position_t position;
    if (kind == "position" || value.contains ("position"))
        position.position = integer (required (value, "position"), 1);
    if (kind == "position" || value.contains ("estimatedWaitMs"))
        position.estimated_wait_ms = integer (required (value, "estimatedWaitMs"), 0);
    issued_ticket_t ticket;
    if (kind == "ticket" || value.contains ("ticket")) {
        const auto &encoded = text (required (value, "ticket"));
        if (encoded.size () != 66 || !decode_base64url (encoded))
            invalid_message ();
        ticket.ticket = encoded;
    }
    if (kind == "ticket" || value.contains ("expiresInMs"))
        ticket.expires_in_ms = integer (required (value, "expiresInMs"), 0);
    if (kind == "position")
        return {std::move (session), position};
    return {std::move (session), std::move (ticket)};
}

json_t write_node (const node_t &value)
{
    return {{"nodeRid", encode_base64url (value.rid)},
            {"nodeLifecycleGeneration", std::to_string (value.generation)}};
}

json_t write_session (const session_t &value)
{
    return {{"streamName", value.stream}, {"sessionRid", encode_base64url (value.rid)}};
}

json_t write_connection (const connection_t &value)
{
    auto output = write_node (node_of (value));
    output.update (write_session (session_of (value)));
    return output;
}

json_t write_delivery (const delivery_t &value)
{
    auto output = client_payload (value.value);
    output.update (write_session (value.session));
    output["kind"] = std::holds_alternative<position_t> (value.value) ? "position" : "ticket";
    return output;
}

template <typename Range, typename Fn> json_t write_list (const Range &values, Fn write)
{
    auto output = json_t::array ();
    for (const auto &value : values)
        output.push_back (write (value));
    return output;
}

} // namespace

std::optional<std::string> reply_packet_name (std::string_view request)
{
    if (request == enqueue_packet || request == report_packet || request == admit_packet)
        return std::string (request).append (".reply");
    return {};
}

json_t write_enqueue (const connection_t &value)
{
    return {{"connection", write_connection (value)}};
}

connection_t read_enqueue (const json_t &value)
{
    return read_connection (required (value, "connection"));
}

json_t write_admission_message (const enqueue_reply_t &value)
{
    return {{"epoch", std::to_string (value.epoch)}, {"delivery", write_delivery (value.delivery)}};
}

enqueue_reply_t read_enqueue_reply (const json_t &value)
{
    return {decimal (required (value, "epoch")), read_delivery (required (value, "delivery"))};
}

json_t write_admission_message (const node_report_t &value)
{
    auto output = write_node (value.node);
    output["limit"] = value.limit;
    output["inFlight"] = value.in_flight;
    output["usedTickets"] = write_list (value.used, write_session);
    output["endedSessions"] = write_list (value.ended, write_session);
    output["heldPermits"] = write_list (value.held, [] (auto id) { return std::to_string (id); });
    output["resolvedRequestSeq"] = std::to_string (value.resolved_request_seq);
    return output;
}

node_report_t read_report (const json_t &value)
{
    return {read_node (value),
            integer (required (value, "limit"), 1),
            integer (required (value, "inFlight"), 0),
            list (required (value, "usedTickets"), read_session),
            list (required (value, "endedSessions"), read_session),
            list (required (value, "heldPermits"), [] (const auto &id) { return decimal (id); }),
            decimal (required (value, "resolvedRequestSeq"), true)};
}

json_t write_admission_message (const report_reply_t &value)
{
    return {{"epoch", std::to_string (value.epoch)},
            {"deliveries", write_list (value.deliveries, write_delivery)}};
}

report_reply_t read_report_reply (const json_t &value)
{
    return {decimal (required (value, "epoch")),
            list (required (value, "deliveries"), read_delivery)};
}

json_t write_admission_message (const admit_request_t &value)
{
    auto output = write_connection (value.connection);
    output["ownerId"] = value.owner.id;
    output["leaseGeneration"] = std::to_string (value.owner.generation);
    output["epoch"] = std::to_string (value.epoch);
    output["requestSeq"] = std::to_string (value.request_seq);
    return output;
}

admit_request_t read_admit (const json_t &value)
{
    const auto &owner = text (required (value, "ownerId"));
    if (owner.empty ())
        invalid_message ();
    return {read_connection (value),
            {owner, decimal (required (value, "leaseGeneration"))},
            decimal (required (value, "epoch")),
            decimal (required (value, "requestSeq"))};
}

json_t write_admission_message (const release_t &value)
{
    auto output = write_node (value.node);
    output["permitId"] = std::to_string (value.permit_id);
    return output;
}

release_t read_release (const json_t &value)
{
    return {read_node (value), decimal (required (value, "permitId"))};
}

json_t write_admit_reply (std::uint64_t permit_id)
{
    if (permit_id)
        return {{"granted", true}, {"permitId", std::to_string (permit_id)}};
    return {{"granted", false}};
}

std::uint64_t read_admit_reply (const json_t &value)
{
    const auto &granted = required (value, "granted");
    if (!granted.is_boolean ())
        invalid_message ();
    if (granted.get<bool> ())
        return decimal (required (value, "permitId"));
    if (value.contains ("permitId"))
        (void) decimal (required (value, "permitId"), true);
    return 0;
}

json_t client_payload (const delivery_value_t &value)
{
    if (const auto *position = std::get_if<position_t> (&value))
        return {{"position", position->position}, {"estimatedWaitMs", position->estimated_wait_ms}};
    const auto &ticket = std::get<issued_ticket_t> (value);
    return {{"ticket", ticket.ticket}, {"expiresInMs", ticket.expires_in_ms}};
}

} // namespace zlink::framework::runtime::admission_queue
