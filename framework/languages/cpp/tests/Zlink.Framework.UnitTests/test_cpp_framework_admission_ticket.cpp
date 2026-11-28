/* SPDX-License-Identifier: Elastic-2.0 */
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <stdexcept>

#include "runtime/streams/admission_ticket.hpp"

using namespace zlink::framework::runtime::admission_queue;
using nlohmann::json;

static void check (bool condition, const json &entry)
{
    if (!condition)
        throw std::runtime_error (entry.at ("name").get<std::string> ());
}

static void run (const json &vectors)
{
    if (vectors.size () != 12)
        throw std::runtime_error ("Expected twelve ticket vectors");
    for (const auto &entry : vectors) {
        const auto &given = entry.at ("given");
        auto read_connection = [&] (bool verification) {
            auto field = [&] (const char *issue, const char *verify) -> const json & {
                return given.at (verification ? verify : issue);
            };
            return connection_t{
              *decode_base64url (
                field ("nodeRidBase64Url", "verificationNodeRidBase64Url").get<std::string> ()),
              field ("nodeLifecycleGeneration", "verificationNodeLifecycleGeneration")
                .get<std::uint64_t> (),
              field ("streamName", "verificationStreamName").get<std::string> (),
              *decode_base64url (field ("sessionRidBase64Url", "verificationSessionRidBase64Url")
                                   .get<std::string> ())};
        };
        const auto previous = given.at ("previousKeyBase64Url");
        const auto expected = entry.at ("expect").at ("accept").get<bool> ();
        try {
            const ticket_signer_t signer (
              given.at ("keyBase64Url").get<std::string> (),
              previous.is_null () ? std::nullopt : std::optional (previous.get<std::string> ()));
            const auto bytes = entry.at ("ticket").get<std::string> ();
            const auto verified = signer.verify (
              read_connection (true), bytes, given.at ("verificationEpoch").get<std::uint64_t> (),
              given.at ("nowUnixMs").get<std::uint64_t> ());
            check (verified == expected, entry);
            check (entry.at ("expect").at ("reason") != "keyTooShort", entry);
            if (entry.at ("name") == "current-key") {
                check (signer.issue (read_connection (false),
                                     given.at ("epoch").get<std::uint64_t> (),
                                     given.at ("expiresAtUnixMs").get<std::uint64_t> ())
                         == bytes,
                       entry);
            }
        }
        catch (const std::invalid_argument &) {
            check (!expected && entry.at ("expect").at ("reason") == "keyTooShort", entry);
        }
        std::cout << "PASS " << entry.at ("name").get<std::string> () << '\n';
    }
}

int main ()
{
    try {
        const auto fixture = nlohmann::json::parse (std::ifstream (ZLINK_ADMISSION_QUEUE_PATH));
        run (fixture.at ("ticketVectors"));
    }
    catch (const std::exception &error) {
        std::cerr << "FAIL " << error.what () << '\n';
        return 1;
    }
}
