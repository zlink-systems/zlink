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
#include <iostream>
#include <mutex>
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
        (void) connector.close ();
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
