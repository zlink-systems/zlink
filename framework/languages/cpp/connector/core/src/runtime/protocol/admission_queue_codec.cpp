/* SPDX-License-Identifier: Apache-2.0 */
#include "runtime/protocol/admission_queue_codec.hpp"

#include <algorithm>
#include <limits>
#include <nlohmann/json.hpp>
#include <zlink/detail/admission_ticket_wire.hpp>

namespace zlink::stream_connector::detail
{
namespace
{
using json = nlohmann::json;

std::optional<std::int32_t> integer_field (const json &body, const char *name, std::int32_t min)
{
    const auto field = body.find (name);
    if (field == body.end () || !field->is_number_integer ()) {
        return std::nullopt;
    }
    constexpr auto max = std::numeric_limits<std::int32_t>::max ();
    if (field->is_number_unsigned ()) {
        const auto value = field->get<std::uint64_t> ();
        if (value <= static_cast<std::uint64_t> (max)
            && value >= static_cast<std::uint64_t> (min)) {
            return static_cast<std::int32_t> (value);
        }
    } else {
        const auto value = field->get<std::int64_t> ();
        if (value >= min && value <= max) {
            return static_cast<std::int32_t> (value);
        }
    }
    return std::nullopt;
}

bool valid_ticket (std::string_view ticket)
{
    constexpr std::string_view alphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    // The second symbol completes version 1 and the final symbol has four zero padding bits.
    return ticket.size () == zlink::detail::admission_ticket_encoded_size && ticket.front () == 'A'
           && alphabet.substr (16, 16).find (ticket[1]) != std::string_view::npos
           && std::string_view ("AQgw").find (ticket.back ()) != std::string_view::npos
           && std::all_of (ticket.begin () + 2, ticket.end () - 1,
                           [alphabet] (char ch) { return alphabet.find (ch) != alphabet.npos; });
}
} // namespace

std::optional<admission_update_t> decode_admission_update (const std::vector<std::uint8_t> &payload,
                                                           admission_update_kind_t kind)
{
    std::size_t fields = 0;
    const auto count_fields = [&] (int depth, json::parse_event_t event, json &) {
        if (depth == 1 && event == json::parse_event_t::key) {
            ++fields;
        }
        return true;
    };
    const auto body = json::parse (payload, count_fields, false);
    if (!body.is_object () || fields != 2 || body.size () != 2) {
        return std::nullopt;
    }
    if (kind != admission_update_kind_t::ticket) {
        const auto position = integer_field (body, "position", 1);
        const auto wait = integer_field (body, "estimatedWaitMs", 0);
        if (position && wait) {
            return admission_position_t{*position, std::chrono::milliseconds (*wait)};
        }
    }
    if (kind != admission_update_kind_t::position) {
        const auto ticket = body.find ("ticket");
        if (ticket != body.end () && ticket->is_string ()
            && integer_field (body, "expiresInMs", 1)) {
            const auto &value = ticket->get_ref<const std::string &> ();
            if (valid_ticket (value)) {
                return value;
            }
        }
    }
    return std::nullopt;
}

} // namespace zlink::stream_connector::detail
