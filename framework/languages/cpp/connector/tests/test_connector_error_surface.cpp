/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#include <zlink/stream_connector_throwing.hpp>
#include <zlink/stream_e2e_client/coroutine.hpp>

#include <boost/asio/ip/tcp.hpp>
#include <chrono>
#include <future>
#include <iostream>
#include <string_view>
#include <thread>

namespace sc = zlink::stream_connector;
namespace se = zlink::stream_e2e_client;
namespace st = zlink::stream_connector_throwing;

bool preserves (sc::error_code_t code, const auto &operation)
{
    try {
        operation ();
    }
    catch (const st::stream_connector_error &error) {
        return error.code () == code && std::string_view (error.what ()) == "operation failed";
    }
    catch (const std::exception &error) {
        std::cerr << "missing connector code " << static_cast<int> (code) << ": " << error.what ()
                  << '\n';
    }
    return false;
}

se::task_t<int> nested_failure (sc::error_code_t code)
{
    auto inner = se::task_t<int> (
      [code] (auto callback) { callback (sc::result_t<int>::failure (code, "operation failed")); });
    co_return co_await inner;
}

se::task_t<void> application_failure ()
{
    throw std::runtime_error ("application failed");
    co_return;
}

int main (int argc, char **argv)
{
    const std::string_view mode = argc > 1 ? argv[1] : "task-value";
    bool codes_preserved = true;
    for (const auto code : {sc::error_code_t::validation_failed, sc::error_code_t::disconnected}) {
        if (mode == "task-value") {
            auto task = se::task_t<int> ([code] (auto callback) {
                callback (sc::result_t<int>::failure (code, "operation failed"));
            });
            codes_preserved =
              preserves (code, [&] { (void) task.await_resume (); }) && codes_preserved;
        } else if (mode == "task-void") {
            auto task = se::task_t<void> ([code] (auto callback) {
                callback (sc::result_t<void>::failure (code, "operation failed"));
            });
            codes_preserved = preserves (code, [&] { task.await_resume (); }) && codes_preserved;
        } else if (mode == "nested") {
            auto task = nested_failure (code);
            const auto result = task.consume_result ();
            if (result || result.error_code () != code
                || result.error ()->message != "operation failed") {
                std::cerr << "nested lost connector code " << static_cast<int> (code) << '\n';
                codes_preserved = false;
            }
        }
    }
    if (mode == "task-value" || mode == "task-void" || mode == "nested") {
        if (st::value_or_throw (sc::result_t<int>::success (7)) != 7)
            return 2;
        st::value_or_throw (sc::result_t<void>::success ());
        auto application = application_failure ();
        const auto application_result = application.consume_result ();
        if (application_result
            || application_result.error_code () != sc::error_code_t::user_callback_failed)
            return 4;
        return codes_preserved ? 0 : 1;
    }
    if (mode != "core-future" && mode != "e2e-future")
        return 3;
    boost::asio::io_context io;
    boost::asio::ip::tcp::acceptor acceptor (io, {boost::asio::ip::make_address ("127.0.0.1"), 0});
    std::promise<void> release;
    auto released = release.get_future ();
    std::thread server ([&] {
        boost::asio::ip::tcp::socket socket (io);
        acceptor.accept (socket);
        released.wait ();
    });
    sc::connector_options_t options;
    options.endpoint = "tcp://127.0.0.1:" + std::to_string (acceptor.local_endpoint ().port ());
    options.reconnect.enabled = false;
    options.dispatch_mode = sc::dispatch_mode_t::manual;
    auto connector = sc::connector_factory_t::create (options);
    bool passed = static_cast<bool> (connector.connect ());
    if (passed) {
        for (const auto code :
             {sc::error_code_t::validation_failed, sc::error_code_t::disconnected}) {
            if (code == sc::error_code_t::disconnected)
                (void) connector.close ();
            auto call =
              connector.wait_for<sc::packet_t> ("absent").timeout (std::chrono::milliseconds (1));
            auto future = mode == "core-future"
                            ? call.to_future ("operation failed")
                            : se::coroutine_wait_call_t<sc::packet_t> (std::move (call))
                                .to_future ("operation failed");
            const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (2);
            while (future.wait_for (std::chrono::milliseconds (0)) != std::future_status::ready
                   && std::chrono::steady_clock::now () < deadline) {
                (void) connector.dispatch ();
                std::this_thread::yield ();
            }
            if (future.wait_for (std::chrono::milliseconds (0)) != std::future_status::ready
                || !preserves (code, [&] { (void) future.get (); })) {
                passed = false;
            }
        }
    }
    (void) connector.close ();
    release.set_value ();
    server.join ();
    return passed ? 0 : 1;
}
