/* SPDX-License-Identifier: Elastic-2.0 */
#include "runtime/streams/admission_queue_messages.hpp"

#include <fstream>
#include <iostream>
#include <stdexcept>
#include <zlink/framework/contracts/errors/result.hpp>
#include <zlink/framework/codecs/json.hpp>

using namespace zlink::framework::runtime::admission_queue;

namespace
{

json_t canonical_request (const std::string &packet, const json_t &value)
{
    if (packet == enqueue_packet)
        return write_enqueue (read_enqueue (value));
    if (packet == report_packet)
        return write_admission_message (read_report (value));
    if (packet == admit_packet)
        return write_admission_message (read_admit (value));
    if (packet == release_packet)
        return write_admission_message (read_release (value));
    throw std::runtime_error ("Unknown admission message in fixture");
}

json_t canonical_reply (const std::string &packet, const json_t &value)
{
    if (packet == enqueue_packet)
        return write_admission_message (read_enqueue_reply (value));
    if (packet == report_packet)
        return write_admission_message (read_report_reply (value));
    if (packet == admit_packet)
        return write_admit_reply (read_admit_reply (value));
    throw std::runtime_error ("Unknown admission reply in fixture");
}

json_t typed_json_round_trip (const json_t &value)
{
    return zlink::message_t::from_json (value).parse_json<json_t> ();
}

void check (bool passed, const json_t &fixture)
{
    if (!passed)
        throw std::runtime_error (fixture.at ("name").get<std::string> ());
}

} // namespace

int main ()
{
    try {
        const auto fixture = json_t::parse (std::ifstream (ZLINK_ADMISSION_MESSAGES_PATH));
        if (fixture.at ("messages").size () < 7 || fixture.at ("invalid").size () < 7
            || fixture.at ("spotType") != spot_type)
            throw std::runtime_error ("Unexpected admission message fixture contract");
        for (const auto &entry : fixture.at ("messages")) {
            const auto packet = entry.at ("packetName").get<std::string> ();
            const auto &request = entry.at (packet == release_packet ? "send" : "request");
            check (
              typed_json_round_trip (canonical_request (packet, typed_json_round_trip (request)))
                == request,
              entry);
            if (entry.contains ("replyPacketName"))
                check (reply_packet_name (packet)
                         == std::optional (entry.at ("replyPacketName").get<std::string> ()),
                       entry);
            if (entry.contains ("response"))
                check (typed_json_round_trip (
                         canonical_reply (packet, typed_json_round_trip (entry.at ("response"))))
                         == entry.at ("response"),
                       entry);
            std::cout << "PASS " << entry.at ("name").get<std::string> () << '\n';
        }
        for (const auto &entry : fixture.at ("invalid")) {
            const auto packet = entry.at ("packetName").get<std::string> ();
            const auto accepted = entry.at ("expect") == "accepted";
            try {
                if (entry.contains ("payload")) {
                    const auto canonical =
                      canonical_request (packet, typed_json_round_trip (entry.at ("payload")));
                    check (accepted && !canonical.contains ("future"), entry);
                } else {
                    (void) canonical_reply (packet,
                                            typed_json_round_trip (entry.at ("responsePayload")));
                    check (accepted, entry);
                }
            }
            catch (const zlink::framework::framework_exception_t &error) {
                check (!accepted
                         && error.kind ()
                              == zlink::framework::framework_error_kind_t::protocol_error,
                       entry);
            }
            std::cout << "PASS " << entry.at ("name").get<std::string> () << '\n';
        }
    }
    catch (const std::exception &error) {
        std::cerr << "FAIL " << error.what () << '\n';
        return 1;
    }
}
