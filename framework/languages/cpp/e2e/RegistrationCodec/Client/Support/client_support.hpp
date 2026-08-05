/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../Shared/registration_codec_contracts.hpp"

#include <zlink/http_client.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace zlink::framework::e2e::registration_codec::client
{

struct client_options_t
{
    std::string scenario;
    std::string api_endpoint;
    std::string http_endpoint;
    std::string invalid_server_executable;
    std::string invalid_endpoint;
    std::string log_dir;
    std::string config_dir;
};

inline client_options_t read_client_options (int argc, char **argv)
{
    std::string path;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        constexpr std::string_view prefix = "--config=";
        if (argument.rfind (prefix, 0) != 0) {
            throw std::runtime_error ("unknown RegistrationCodec client option: " + argument);
        }
        path = argument.substr (prefix.size ());
    }
    if (path.empty ()) {
        throw std::runtime_error ("RegistrationCodec client requires --config=<path>");
    }
    std::ifstream input (path);
    if (!input) {
        throw std::runtime_error ("cannot open RegistrationCodec client config: " + path);
    }
    const auto section = nlohmann::json::parse (input).at ("e2e");
    const auto value = [&] (const char *key) {
        const auto found = section.find (key);
        return found == section.end () ? std::string{} : found->get<std::string> ();
    };
    return {.scenario = value ("scenario"),
            .api_endpoint = value ("apiEndpoint"),
            .http_endpoint = value ("httpEndpoint"),
            .invalid_server_executable = value ("invalidServerExecutable"),
            .invalid_endpoint = value ("invalidEndpoint"),
            .log_dir = value ("logDir"),
            .config_dir = value ("configDir")};
}

inline void ensure (bool condition, const std::string &message)
{
    if (!condition) {
        throw std::runtime_error (message);
    }
}

inline evidence_snapshot_t fetch_evidence (const std::string &base_url)
{
    auto client = zlink::http_client::client_t::create ()
                    .base_url (base_url)
                    .timeout (std::chrono::milliseconds (1000))
                    .build ();
    return client.get ("/evidence").submit<evidence_snapshot_t> ().result ().value ().body;
}

inline bool snapshot_contains (const evidence_snapshot_t &snapshot,
                               const std::string &marker,
                               const std::string &value)
{
    return std::any_of (snapshot.entries.begin (), snapshot.entries.end (),
                        [&] (const evidence_entry_t &entry) {
                            return entry.marker == marker && entry.value == value;
                        });
}

inline evidence_snapshot_t wait_evidence_contains (const std::string &base_url,
                                                   const std::string &marker,
                                                   const std::string &value,
                                                   std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now () + timeout;
    while (std::chrono::steady_clock::now () < deadline) {
        auto snapshot = fetch_evidence (base_url);
        if (std::any_of (snapshot.entries.begin (), snapshot.entries.end (),
                         [&] (const evidence_entry_t &entry) {
                             return entry.marker == marker && entry.value == value;
                         })) {
            return snapshot;
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (100));
    }
    throw std::runtime_error ("timed out waiting for evidence " + marker + "=" + value);
}

template <typename TReply>
inline TReply post_empty (const std::string &base_url,
                          const std::string &path,
                          std::chrono::milliseconds timeout = std::chrono::seconds (30))
{
    auto client =
      zlink::http_client::client_t::create ().base_url (base_url).timeout (timeout).build ();
    return client.post (path).template submit<TReply> ().result ().value ().body;
}

} // namespace zlink::framework::e2e::registration_codec::client
