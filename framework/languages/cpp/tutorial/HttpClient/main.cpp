#include "../Shared/contracts.hpp"

#include <zlink/http_client.hpp>

#include <chrono>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace hc = zlink::http_client;
namespace fw = zlink::framework;

struct weight_response_t
{
    std::string channel;
    int weight = 0;
};

struct import_response_t
{
    int imported = 0;
};

inline void from_json (const nlohmann::json &json, weight_response_t &value)
{
    value.channel = json.value ("channel", "");
    value.weight = json.value ("weight", 0);
}

inline void from_json (const nlohmann::json &json, import_response_t &value)
{
    value.imported = json.value ("imported", 0);
}

const char *kind_name (fw::framework_error_kind_t kind)
{
    switch (kind) {
        case fw::framework_error_kind_t::internal_failure:
            return "internal_failure";
        case fw::framework_error_kind_t::unavailable:
            return "unavailable";
        case fw::framework_error_kind_t::protocol_error:
            return "protocol_error";
        default:
            return "other";
    }
}

fw::task_t<int> run ()
{
    try {
        // --8<-- [start:http-client-create]
        // The client is shared by the sequential examples and destroyed at program exit.
        auto client = hc::client_t::create ("http://127.0.0.1:5180")
                        .timeout (std::chrono::seconds (3))
                        .build ();
        // --8<-- [end:http-client-create]

        // --8<-- [start:http-first-request]
        // The CLI awaits the asynchronous terminator before printing its response.
        const auto first = co_await client.get ("/players/p1/profile").async<player_profile_t> ();
        std::cout << "first request: " << first.body.player_id << " " << first.body.nickname
                  << std::endl;
        // --8<-- [end:http-first-request]

        // --8<-- [start:http-request-shaping]
        // The request overrides both a header and its timeout; the admin client has another
        // base URL.
        const auto status = co_await client.get ("/ops/nodes/game-server-1/status")
                              .header ("x-trace-id", "tutorial-1")
                              .timeout (std::chrono::seconds (5))
                              .async<node_status_t> ();
        auto admin = hc::client_t::create ("http://127.0.0.1:5181")
                       .basic_auth ("ops", "tutorial-admin")
                       .build ();
        const auto weight = co_await admin.post ("/admin/channels/profile/weight")
                              .query ("value", "2")
                              .async<weight_response_t> ();
        std::cout << "request shaping: status " << status.status << " weight " << weight.body.weight
                  << std::endl;
        // --8<-- [end:http-request-shaping]

        // --8<-- [start:http-json-body]
        // The room id is state shared by the room create, chat, and later room requests.
        const auto player = co_await client.post ("/players/p2")
                              .body (create_player_t{"rookie"})
                              .async<std::string> ();
        const auto room = co_await client.post ("/rooms")
                            .body (open_room_t{"tutorial-room"})
                            .async<std::string> ();
        const auto chat = co_await client.post ("/rooms/" + room.body + "/chat")
                            .body (post_chat_t{"p2", "hello"})
                            .async_raw ();
        const auto room_id = room.body;
        std::cout << "json body: player " << player.status << " room " << room_id << " chat "
                  << chat.status << std::endl;
        // --8<-- [end:http-json-body]

        // --8<-- [start:http-response-kinds]
        // typed keeps the envelope, raw keeps headers/body, and fetch returns only the DTO.
        const auto typed = co_await client.get ("/players/p2").async<player_info_t> ();
        const auto raw = co_await client.get ("/players/p2").async_raw ();
        const auto fetched = co_await client.get ("/players/p2").fetch<player_info_t> ();
        std::cout << "response kinds: typed " << typed.status << " raw "
                  << raw.headers.at ("content-type") << " fetch " << fetched.nickname << std::endl;
        // --8<-- [end:http-response-kinds]

        // --8<-- [start:http-compressed-response]
        // compression() asks the server for gzip and removes content-encoding after decoding.
        auto compressed_client = hc::client_t::create ("http://127.0.0.1:5180")
                                   .compression ()
                                   .build ();
        const auto compressed = co_await compressed_client.get ("/rooms/" + room_id)
                                  .async<room_state_t> ();
        const bool encoding_removed = compressed.headers.count ("content-encoding") == 0;
        std::cout << "compressed response: " << compressed.status << " encoding-removed "
                  << std::boolalpha << encoding_removed << std::endl;
        // --8<-- [end:http-compressed-response]

        // --8<-- [start:http-redirect]
        // followRedirects() follows the path-absolute Location from the legacy route.
        (void) co_await client.post ("/players/p1")
          .body (create_player_t{"rookie"})
          .async<std::string> ();
        auto redirect_client = hc::client_t::create ("http://127.0.0.1:5180")
                                 .follow_redirects ()
                                 .build ();
        const auto redirected = co_await redirect_client.get ("/player/p1").async<player_info_t> ();
        std::cout << "redirect: " << redirected.status << " " << redirected.body.player_id
                  << std::endl;
        // --8<-- [end:http-redirect]

        // --8<-- [start:http-basic-auth]
        // The admin endpoint uses a separate base URL, so two clients show the 401 and 200 paths.
        auto unauthenticated_admin = hc::client_t::create ("http://127.0.0.1:5181").build ();
        const auto without_auth = co_await unauthenticated_admin
                                    .post ("/admin/channels/profile/weight")
                                    .query ("value", "2")
                                    .async_raw ();
        auto authenticated_admin = hc::client_t::create ("http://127.0.0.1:5181")
                                     .basic_auth ("ops", "tutorial-admin")
                                     .build ();
        const auto with_auth = co_await authenticated_admin.post ("/admin/channels/profile/weight")
                                 .query ("value", "2")
                                 .async_raw ();
        std::cout << "basic auth: without " << without_auth.status << " with " << with_auth.status
                  << std::endl;
        // --8<-- [end:http-basic-auth]

        // --8<-- [start:http-download-stream]
        // download() invokes this sink per received chunk; the current host returns one buffered
        // chunk.
        std::size_t chunks = 0;
        std::size_t bytes = 0;
        (void) co_await client.get ("/rooms/" + room_id + "/export")
          .download ([&] (std::string_view chunk) {
              ++chunks;
              bytes += chunk.size ();
          });
        std::cout << "download stream: chunks " << chunks << " bytes " << bytes << std::endl;
        // --8<-- [end:http-download-stream]

        // --8<-- [start:http-upload-stream]
        // body_stream() sends three NDJSON chunks; the host presents its decoded body to the route.
        const std::vector<std::string> import_chunks{
          "{\"playerId\":\"p1\",\"text\":\"import-one\"}\n",
          "{\"playerId\":\"p2\",\"text\":\"import-two\"}\n",
          "{\"playerId\":\"p2\",\"text\":\"import-three\"}\n"};
        std::size_t next_chunk = 0;
        const auto imported = co_await client.post ("/rooms/" + room_id + "/import")
                                .body_stream (
                                  [&] () -> std::optional<std::string> {
                                      if (next_chunk == import_chunks.size ())
                                          return std::nullopt;
                                      return import_chunks[next_chunk++];
                                  },
                                  "application/x-ndjson")
                                .async<import_response_t> ();
        std::cout << "upload stream: imported " << imported.body.imported << std::endl;
        // --8<-- [end:http-upload-stream]

        // --8<-- [start:http-error-kinds]
        // Typed status failures are InternalFailure; a refused TCP connection is Unavailable.
        auto bad_request = fw::framework_error_kind_t::internal_failure;
        try {
            (void) co_await client.post ("/players/p3")
              .body ("not-json", "text/plain")
              .async<player_info_t> ();
        }
        catch (const fw::framework_exception_t &error) {
            bad_request = error.kind ();
        }
        auto closed_port = hc::client_t::create ("http://127.0.0.1:6080").build ();
        auto refused = fw::framework_error_kind_t::internal_failure;
        try {
            (void) co_await closed_port.get ("/players/p1").async_raw ();
        }
        catch (const fw::framework_exception_t &error) {
            refused = error.kind ();
        }
        std::cout << "error kinds: bad request " << kind_name (bad_request)
                  << " connection refused " << kind_name (refused) << std::endl;
        // --8<-- [end:http-error-kinds]

        co_return 0;
    }
    catch (const std::exception &error) {
        std::cerr << "http client failed: " << error.what () << std::endl;
        co_return 1;
    }
}

int main ()
{
    const auto result = run ().result ();
    if (!result) {
        std::cerr << "http client failed: " << result.error ()->what () << std::endl;
        return 1;
    }
    return result.value ();
}
