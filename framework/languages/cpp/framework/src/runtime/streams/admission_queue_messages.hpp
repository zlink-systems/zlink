/* SPDX-License-Identifier: Elastic-2.0 */
#pragma once

#include "runtime/streams/admission_ticket.hpp"

#include <nlohmann/json.hpp>
#include <variant>

namespace zlink::framework::runtime::admission_queue
{

inline constexpr char spot_type[] = "$zlink.AdmissionQueue";
inline constexpr char spot_prefix[] = "$zlink.admission-queue/";
inline constexpr char enqueue_packet[] = "$zlink.admission-queue.enqueue";
inline constexpr char report_packet[] = "$zlink.admission-queue.report";
inline constexpr char admit_packet[] = "$zlink.admission-queue.admit-permit";
inline constexpr char release_packet[] = "$zlink.admission-queue.release-permit";
inline constexpr char client_enqueue_packet[] = "$zlink.admission.enqueue";
inline constexpr char client_position_packet[] = "$zlink.admission.position";
inline constexpr char client_ticket_packet[] = "$zlink.admission.ticket";
inline constexpr char client_ticket_key[] = "$zlink.admission.ticket";
inline constexpr char client_queue_required_key[] = "$zlink.admission.queueRequired";

inline bool is_client_delivery_packet (std::string_view name) noexcept
{
    return name == client_position_packet || name == client_ticket_packet;
}

std::optional<std::string> reply_packet_name (std::string_view request);

using json_t = nlohmann::json;

struct node_t
{
    bytes_t rid;
    std::uint64_t generation{};
    auto operator<=> (const node_t &) const = default;
};

struct session_t
{
    std::string stream;
    bytes_t rid;
    auto operator<=> (const session_t &) const = default;
};

inline node_t node_of (const connection_t &connection)
{
    return {connection.node_rid, connection.node_generation};
}

inline session_t session_of (const connection_t &connection)
{
    return {connection.stream_name, connection.session_rid};
}

inline connection_t connection_of (const node_t &node, const session_t &session)
{
    return {node.rid, node.generation, session.stream, session.rid};
}

struct position_t
{
    std::int32_t position{};
    std::int32_t estimated_wait_ms{};
    auto operator<=> (const position_t &) const = default;
};

struct issued_ticket_t
{
    std::string ticket;
    std::int32_t expires_in_ms{};
    auto operator<=> (const issued_ticket_t &) const = default;
};

using delivery_value_t = std::variant<position_t, issued_ticket_t>;

struct delivery_t
{
    session_t session;
    delivery_value_t value;
};

struct enqueue_reply_t
{
    std::uint64_t epoch{};
    delivery_t delivery;
};

struct node_report_t
{
    node_t node;
    std::int32_t limit{};
    std::int32_t in_flight{};
    std::vector<session_t> used;
    std::vector<session_t> ended;
    std::vector<std::uint64_t> held;
    std::uint64_t resolved_request_seq{};
};

struct report_reply_t
{
    std::uint64_t epoch{};
    std::vector<delivery_t> deliveries;
};

struct owner_t
{
    std::string id;
    std::uint64_t generation{};
    auto operator<=> (const owner_t &) const = default;
};

struct admit_request_t
{
    connection_t connection;
    owner_t owner;
    std::uint64_t epoch{};
    std::uint64_t request_seq{};
};

struct release_t
{
    node_t node;
    std::uint64_t permit_id{};
};

json_t write_enqueue (const connection_t &value);
json_t write_admission_message (const enqueue_reply_t &value);
json_t write_admission_message (const node_report_t &value);
json_t write_admission_message (const report_reply_t &value);
json_t write_admission_message (const admit_request_t &value);
json_t write_admission_message (const release_t &value);
json_t write_admit_reply (std::uint64_t permit_id);
json_t client_payload (const delivery_value_t &value);

connection_t read_enqueue (const json_t &value);
enqueue_reply_t read_enqueue_reply (const json_t &value);
node_report_t read_report (const json_t &value);
report_reply_t read_report_reply (const json_t &value);
admit_request_t read_admit (const json_t &value);
release_t read_release (const json_t &value);
std::uint64_t read_admit_reply (const json_t &value);

} // namespace zlink::framework::runtime::admission_queue
