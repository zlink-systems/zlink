/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/backend/raw_dealer_port.hpp"
#include "runtime/messaging/submit_result_mapper.hpp"
#include "runtime/mesh/user_spot_terminal_mapping.hpp"

#include <zlink.hpp>

#include <chrono>
#include <iostream>
#include <utility>

namespace backend = zlink::framework::detail::backend;
namespace messaging = zlink::framework::runtime::messaging;

int main ()
{
    using namespace std::chrono_literals;
    using zlink::framework::framework_error_kind_t;
    constexpr auto request_timeout = 25ms;
    zlink::context_t context;
    zlink::dealer_socket_t socket (context);
    socket.options ().linger (0ms);
    backend::raw_dealer_port_t port (socket);
    context.shutdown ();
    auto request = port.request (backend::raw_message_t{{'r', 'e', 'q'}}, request_timeout);
    if (!request.await_ready ()) {
        std::cerr << "request did not settle synchronously\n";
        return 1;
    }
    const auto &settled = request.result ();
    if (!settled) {
        std::cerr << "terminated DEALER request lost typed Terminated: kind="
                  << static_cast<int> (settled.error_kind ()) << '\n';
        return 1;
    }
    const auto &completion = settled.value ();
    if (completion.result != backend::raw_request_result_t::terminated || !completion.failure
        || completion.failure->phase != backend::raw_request_failure_phase_t::initial_admission
        || completion.failure->submit_result != zlink::submit_result_t::terminated
        || messaging::map_submit_result_exception (*completion.failure->submit_result,
                                                   "DEALER request")
               .kind ()
             != framework_error_kind_t::shutting_down) {
        std::cerr << "DEALER completion did not preserve typed shutdown failure\n";
        return 1;
    }
    port.close ();

    zlink::context_t invalid_context;
    zlink::dealer_socket_t invalid_socket (invalid_context);
    invalid_socket.options ().linger (0ms);
    backend::raw_dealer_port_t invalid_port (invalid_socket);
    invalid_socket.close ();
    auto invalid_request = invalid_port.request (backend::raw_message_t{{'r'}}, request_timeout);
    if (!invalid_request.await_ready () || !invalid_request.result ()
        || !invalid_request.result ().value ().failure) {
        std::cerr << "closed DEALER did not retain its typed caller failure\n";
        return 1;
    }
    const auto &invalid_failure = *invalid_request.result ().value ().failure;
    const auto user_spot_kind =
      zlink::framework::runtime::user_spot_terminal::map_user_spot_operation_failure (
        zlink::framework::runtime::foundation::operation_terminal_t::completed,
        {1, static_cast<std::uint32_t> (invalid_failure.terminal_result ()),
         static_cast<std::uint32_t> (
           zlink::framework::runtime::protocol::framework_error_code::none)},
        true);
    if (user_spot_kind != framework_error_kind_t::invalid_operation) {
        std::cerr << "User Spot lost Core caller failure: kind="
                  << static_cast<int> (user_spot_kind) << '\n';
        return 1;
    }
    invalid_port.close ();

    for (const auto &[result, expected] :
         {std::pair{zlink::submit_result_t::not_admitted, framework_error_kind_t::rejected},
          std::pair{zlink::submit_result_t::invalid_argument,
                    framework_error_kind_t::invalid_operation},
          std::pair{zlink::submit_result_t::invalid_handle,
                    framework_error_kind_t::invalid_operation},
          std::pair{zlink::submit_result_t::invalid_state,
                    framework_error_kind_t::invalid_operation},
          std::pair{zlink::submit_result_t::thread_violation,
                    framework_error_kind_t::invalid_operation},
          std::pair{zlink::submit_result_t::terminated, framework_error_kind_t::shutting_down},
          std::pair{zlink::submit_result_t::not_found, framework_error_kind_t::not_found}}) {
        if (messaging::map_submit_result_exception (result, "typed submit").kind () != expected) {
            std::cerr << "typed submit kind mismatch for result " << static_cast<int> (result)
                      << '\n';
            return 1;
        }
    }
    namespace protocol = zlink::framework::runtime::protocol;
    const messaging::request_failure_mapper_t request_mapper;
    for (const auto &[terminal, expected_code] :
         {std::pair{zlink::request_result_t::not_found,
                    protocol::framework_error_code::requestTargetNotFound},
          std::pair{zlink::request_result_t::protocol_error,
                    protocol::framework_error_code::requestProtocolError},
          std::pair{zlink::request_result_t::internal_error,
                    protocol::framework_error_code::requestFailed},
          std::pair{zlink::request_result_t::rejected,
                    protocol::framework_error_code::requestRejected},
          std::pair{zlink::request_result_t::invalid_argument,
                    protocol::framework_error_code::none}}) {
        const auto code = request_mapper.reply_failure_code (static_cast<std::uint32_t> (terminal));
        if (expected_code != protocol::framework_error_code::none
            && protocol::valid_terminal_failure (static_cast<std::uint32_t> (terminal),
                                                 protocol::framework_error_code::none)) {
            std::cerr << "schema accepted a typed terminal without its required failure code\n";
            return 1;
        }
        if (code != static_cast<std::uint32_t> (expected_code)
            || !protocol::valid_terminal_failure (static_cast<std::uint32_t> (terminal),
                                                  expected_code)) {
            std::cerr << "typed terminal lost its schema failure code\n";
            return 1;
        }
    }
    for (const auto terminal : {zlink::request_result_t::conflict, zlink::request_result_t::busy,
                                zlink::request_result_t::backpressured}) {
        const backend::raw_request_completion_t legacy{
          backend::raw_request_result_t::failed,
          {},
          backend::raw_request_failure_t{backend::raw_request_failure_phase_t::completion_terminal,
                                         std::nullopt, terminal, 0}};
        if (legacy.has_unrepresented_typed_result ()) {
            std::cerr << "legacy Unavailable was changed into a synthetic application reply\n";
            return 1;
        }
    }
    std::cout << "PASS public DEALER typed shutdown failure and seven submit error kinds\n";
}
