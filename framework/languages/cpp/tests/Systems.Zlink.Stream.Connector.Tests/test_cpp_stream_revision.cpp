/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#include <zlink/stream_connector/contracts/zlink_stream_assert.hpp>
#include <zlink/stream_connector.hpp>
#include "runtime/connector_runtime.hpp"
#include "runtime/protocol/header_codec.hpp"
#include "runtime/transport/transport_connection.hpp"
#include "runtime/transport/stream_connection.hpp"

#include <boost/asio/post.hpp>
#include <condition_variable>
#include <future>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#ifndef _WIN32
#include <unistd.h>
#endif

using namespace zlink::stream_connector;

struct empty_named_payload_t
{
    static constexpr std::string_view packet_name = "";
};
std::vector<std::uint8_t> to_stream_payload (const empty_named_payload_t &)
{
    return {'{', '}'};
}

class blocked_connection_t final : public detail::stream_connection_t
{
  public:
    void async_read_some (
      std::size_t,
      std::function<void (boost::system::error_code, std::vector<std::uint8_t>)>) override
    {
    }
    void async_write (std::vector<std::uint8_t>,
                      std::function<void (boost::system::error_code)> completion) override
    {
        std::lock_guard lock (mutex);
        ++writes;
        pending = std::move (completion);
        changed.notify_all ();
    }
    boost::system::error_code shutdown_and_close () override
    {
        std::lock_guard lock (mutex);
        pending = {};
        return {};
    }
    bool await_write ()
    {
        std::unique_lock lock (mutex);
        return changed.wait_for (lock, std::chrono::seconds (5), [&] { return writes != 0; });
    }
    void finish_write ()
    {
        std::function<void (boost::system::error_code)> completion;
        {
            std::lock_guard lock (mutex);
            completion = std::move (pending);
        }
        if (completion)
            completion ({});
    }
    std::mutex mutex;
    std::condition_variable changed;
    std::size_t writes = 0;
    std::function<void (boost::system::error_code)> pending;
};

int main ()
{
    int failures = 0;
    auto check = [&] (bool condition, const char *name) {
        std::cout << name << ": " << (condition ? "PASS" : "FAIL") << '\n';
        failures += !condition;
    };
    detail::stream_header_t header;
    header.name = " \t\r\n";
    check (!detail::header_codec_t{}.encode (header), "R06 whitespace packet name");
    header.name = "valid";
    check (static_cast<bool> (detail::header_codec_t{}.encode (header)), "R06 valid packet name");
    auto named_connector = connector_factory_t::create (connector_options_t{});
    auto fixture_actor = detail::actor_access_t::create (
      connector_internal_handle (named_connector), "r4.fixture.actor", 1);
    std::ifstream fixture (ZLINK_STREAM_PACKET_NAME_FIXTURE_PATH);
    check (fixture.is_open (), "R4 common whitespace fixture opened");
    std::string row;
    std::size_t fixture_rows = 0;
    while (std::getline (fixture, row)) {
        if (row.empty () || row.front () == '#')
            continue;
        const auto tab = row.find ('\t');
        check (tab != std::string::npos, "R4 fixture delimiter");
        if (tab == std::string::npos)
            continue;
        if (row.back () == '\r')
            row.pop_back ();
        const auto expectation = row.substr (tab + 1);
        check (expectation == "true" || expectation == "false", "R4 fixture expectation");
        const auto blank = row.substr (tab + 1).starts_with ("true");
        ++fixture_rows;
        std::string name;
        std::istringstream points (row.substr (0, tab));
        std::string point;
        while (std::getline (points, point, ',') && point != "EMPTY") {
            const auto cp = std::stoul (point, nullptr, 16);
            if (cp < 0x80)
                name += static_cast<char> (cp);
            else if (cp < 0x800) {
                name += static_cast<char> (0xc0 | (cp >> 6));
                name += static_cast<char> (0x80 | (cp & 0x3f));
            } else {
                name += static_cast<char> (0xe0 | (cp >> 12));
                name += static_cast<char> (0x80 | ((cp >> 6) & 0x3f));
                name += static_cast<char> (0x80 | (cp & 0x3f));
            }
        }
        header.name = name;
        const auto label = "R4 fixture " + row.substr (0, tab);
        check (static_cast<bool> (detail::header_codec_t{}.encode (header)) == !blank,
               label.c_str ());
        int rejected_events = 0;
        auto error_subscription =
          named_connector.on_error ([&] (const zlink::stream_connector::error_t &error) {
              rejected_events += error.code == error_code_t::validation_failed;
          });
        check (named_connector.received_count (name) == 0, "R4 fixture count result");
        auto packet_subscription =
          named_connector.on<std::string> (name, [] (const message_t<std::string> &) {});
        check (packet_subscription.active () == !blank, "R4 fixture subscription result");
        (void) named_connector.dispatch ();
        check (rejected_events == (blank ? 2 : 0), "R4 fixture validation events");
        auto actor_subscription =
          fixture_actor->on<std::string> (name, [] (const message_t<std::string> &) {});
        check (actor_subscription.active () == !blank, "R4 fixture actor subscription result");
        (void) named_connector.dispatch ();
        check (rejected_events == (blank ? 3 : 0), "R4 fixture actor validation event");
    }
    check (fixture_rows != 0, "R4 fixture contains cases");
    int invalid_name_events = 0;
    auto invalid_name_subscription =
      named_connector.on_error ([&] (const zlink::stream_connector::error_t &error) {
          invalid_name_events += error.code == error_code_t::validation_failed;
      });
    check (named_connector.received_count ("") == 0, "R4 rejected count returns zero");
    auto invalid_packet_subscription =
      named_connector.on<std::string> ("", [] (const message_t<std::string> &) {});
    check (!invalid_packet_subscription.active (), "R4 rejected subscription inactive");
    (void) named_connector.dispatch ();
    check (invalid_name_events == 2, "R4 rejected count and on publish ValidationFailed");
    invalid_name_subscription.unsubscribe ();
    check (named_connector.wait_for (" \t\r\n", std::chrono::milliseconds (10)).error_code ()
             == error_code_t::validation_failed,
           "R06 named wait rejects whitespace");
    auto named_state = std::static_pointer_cast<detail::connector_state_t> (
      connector_internal_handle (named_connector));
    auto named_connection = std::make_shared<blocked_connection_t> ();
    named_state->connection = named_connection;
    named_state->state = connection_state_t::connected;
    int named_failures = 0;
    auto named_subscription =
      named_connector.on_error ([&] (const zlink::stream_connector::error_t &error) {
          named_failures += error.code == error_code_t::validation_failed;
      });
    named_connector.send (empty_named_payload_t{}).submit ();
    std::promise<void> named_drained;
    auto named_completion = named_drained.get_future ();
    boost::asio::post (named_state->write_strand, [&] { named_drained.set_value (); });
    named_completion.wait ();
    (void) named_connector.dispatch ();
    check (named_failures == 1 && named_connection->writes == 0,
           "R06 static empty name not replaced by compiler name");
    named_connector.send (packet_t{.name = "", .payload = {3}}).submit ();
    (void) named_connector.dispatch ();
    check (named_failures == 2, "R06 raw empty send name rejected");
    auto raw_request = named_connector.request (packet_t{.name = "", .payload = {4}})
                         .timeout (std::chrono::milliseconds (10))
                         .submit<std::string> ();
    check (raw_request.error_code () == error_code_t::validation_failed,
           "R06 raw empty request name rejected");

    struct action_error_t : std::runtime_error
    {
        using std::runtime_error::runtime_error;
    };
    bool propagated = false;
    try {
        (void) assertions::expect_failure (
          [] () -> result_t<void> { throw action_error_t ("action failure"); });
    }
    catch (const action_error_t &error) {
        propagated = std::string_view (error.what ()) == "action failure";
    }
    check (propagated, "assert action exception propagation");
    propagated = false;
    try {
        (void) assertions::expect_timeout (
          [] () -> result_t<void> { throw action_error_t ("connect timed out"); });
    }
    catch (const action_error_t &error) {
        propagated = std::string_view (error.what ()) == "connect timed out";
    }
    check (propagated, "R4 uncoded timeout-like exception propagation");
    struct coded_action_error_t : assertions::failure_t
    {
        using assertions::failure_t::failure_t;
    };
    const coded_action_error_t *original_coded_failure = nullptr;
    propagated = false;
    try {
        (void) assertions::expect_timeout ([&] () -> result_t<void> {
            try {
                throw coded_action_error_t ({error_code_t::send_failed, "coded failure"});
            }
            catch (const coded_action_error_t &error) {
                original_coded_failure = &error;
                throw;
            }
        });
    }
    catch (const coded_action_error_t &error) {
        propagated = &error == original_coded_failure;
    }
    check (propagated, "R4 coded non-timeout exception identity");
    for (auto code : {error_code_t::request_timeout, error_code_t::connect_timeout}) {
        check (assertions::expect_timeout ([code] {
                   return result_t<void>::failure (code, "coded timeout");
               }).code
                 == code,
               "R4 coded timeout result");
        check (assertions::expect_timeout ([code] () -> result_t<void> {
                   throw assertions::failure_t ({code, "coded timeout"});
               }).code
                 == code,
               "R4 coded timeout exception");
    }
    check (assertions::expect_failure ([] {
               return result_t<void>::failure (error_code_t::send_failed, "expected");
           }).code
             == error_code_t::send_failed,
           "assert returned failure");

    auto state = std::make_shared<detail::connector_state_t> (connector_options_t{});
    auto connection = std::make_shared<blocked_connection_t> ();
    state->connection = connection;
    state->state = connection_state_t::connected;
    detail::submit_send_async (state, packet_t{.name = "blocker", .payload = {1}}, {},
                               std::nullopt);
    if (!connection->await_write ())
        return 2;
    std::promise<error_code_t> terminal;
    auto terminal_result = terminal.get_future ();
    detail::submit_request_async (
      state, packet_t{.name = "expires", .payload = {2}}, std::chrono::milliseconds (10),
      [&] (result_t<detail::request_reply_t> result) {
          terminal.set_value (*result.error_code ());
      },
      true);
    if (terminal_result.wait_for (std::chrono::seconds (5)) != std::future_status::ready)
        return 3;
    check (terminal_result.get () == error_code_t::request_timeout, "G01 request timeout");
    {
        std::lock_guard lock (state->transport_mutex);
        check (state->write_queue.empty (), "G01 terminal request removed from write queue");
        check (state->active_write.has_value (), "G01 active write retained");
    }
    connection->finish_write ();
    std::promise<void> drained;
    auto completion = drained.get_future ();
    boost::asio::post (state->write_strand, [&] { drained.set_value (); });
    completion.wait ();
    {
        std::lock_guard lock (connection->mutex);
        check (connection->writes == 1, "G01 expired frame not written");
    }
#ifndef _WIN32
    for (auto dispatch_mode : {dispatch_mode_t::manual, dispatch_mode_t::immediate}) {
        connector_options_t options;
        options.dispatch_mode = dispatch_mode;
        auto connector = connector_factory_t::create (options);
        auto close_state = std::static_pointer_cast<detail::connector_state_t> (
          connector_internal_handle (connector));
        boost::asio::io_context io;
        boost::asio::ip::tcp::socket socket (io);
        socket.open (boost::asio::ip::tcp::v4 ());
        const auto descriptor = socket.native_handle ();
        io.stop ();
        close_state->connection = detail::make_tcp_connection (io, std::move (socket));
        close_state->state = connection_state_t::connected;
        int close_errors = 0;
        bool observed_closed = false;
        bool observed_client_close = false;
        std::promise<void> error_observed;
        auto error_completion = error_observed.get_future ();
        auto subscription =
          connector.on_error ([&] (const zlink::stream_connector::error_t &error) {
              if (error.code == error_code_t::disconnected) {
                  ++close_errors;
                  observed_closed = connector.state () == connection_state_t::closed;
                  observed_client_close = connector.close_reason () == close_reason_t::client_close;
                  error_observed.set_value ();
              }
          });
        // Invalidate this transport's descriptor to inject a native close failure.
        if (::close (descriptor) != 0)
            return 4;
        check (static_cast<bool> (connector.close ()), "R4 close failure call succeeds");
        (void) detail::dispatch_pending (close_state);
        if (error_completion.wait_for (std::chrono::seconds (5)) != std::future_status::ready)
            return 5;
        check (close_errors == 1, "R12 native close failure reported as Disconnected");
        check (observed_closed, "R12 error handler observes closed state");
        check (observed_client_close, "R12 error handler observes ClientClose reason");
        check (connector.close_reason () == close_reason_t::client_close,
               "R12 close reason preserved");
        check (connector.state () == connection_state_t::closed,
               "R12 failed native close remains closed");
    }
#endif
    return failures != 0;
}
