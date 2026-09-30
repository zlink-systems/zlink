/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#include <zlink/stream_connector.hpp>

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

int main ()
{
    namespace sc = zlink::stream_connector;
    sc::connector_options_t options;
    options.endpoint = "unsupported://localhost";
    options.dispatch_mode = sc::dispatch_mode_t::manual;
    options.reconnect.enabled = false;
    auto connector = sc::connector_factory_t::create (options);
    int callbacks = 0;
    int errors = 0;
    bool original_failure = false;
    auto subscription = connector.on_error ([&] (const sc::error_t &error) {
        if (error.code == sc::error_code_t::user_callback_failed
            && error.message == "manual connect callback failure") {
            ++errors;
        }
    });
    connector.connect ([&] (sc::result_t<void> result) {
        ++callbacks;
        original_failure = result.error_code () == sc::error_code_t::configuration_error;
        throw std::runtime_error ("manual connect callback failure");
    });
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (2);
    while (connector.pending_dispatch_count () == 0
           && std::chrono::steady_clock::now () < deadline) {
        std::this_thread::yield ();
    }
    connector.dispatch ();
    connector.dispatch ();
    connector.close ();
    if (callbacks != 1 || !original_failure || errors != 1) {
        std::cerr << "callbacks=" << callbacks << " original_failure=" << original_failure
                  << " UserCallbackFailed=" << errors << '\n';
        return 1;
    }
    std::cout << "Manual callback failure delivered once as UserCallbackFailed\n";
    return 0;
}
