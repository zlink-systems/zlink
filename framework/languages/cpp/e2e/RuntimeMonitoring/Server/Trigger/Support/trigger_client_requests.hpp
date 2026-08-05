/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#pragma once

#include "trigger_options.hpp"

#include "../../../Shared/runtime_monitoring_contracts.hpp"
#include "../../Shared/evidence_store.hpp"

#include <zlink/framework.hpp>

#include <boost/asio.hpp>

#include <chrono>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

namespace zlink::framework::e2e::runtime_monitoring::trigger
{

class transient_request_service_t final : public zlink::framework::hosted_service_t
{
  public:
    transient_request_service_t (zlink::framework::app_t &app,
                                 profile_req_t request,
                                 std::string trace_label) :
        _app (app), _request (std::move (request)), _trace_label (std::move (trace_label))
    {
    }

    void start (zlink::framework::service_provider_t &services) override
    {
        try {
            auto &channels = services.get_required<zlink::framework::channel_client_t> ();
            auto request = channels.request (profile_channel, _request)
                             .timeout (std::chrono::milliseconds (3000))
                             .submit<profile_res_t> ();
            const auto &reply = request.result ();
            if (!reply.has_value ()) {
                _error =
                  std::string ("transient trigger request failed: ")
                  + (reply.error () ? reply.error ()->what () : "unknown");
            } else {
                _reply = reply.value ();
            }
        }
        catch (const std::exception &ex) {
            _error = ex.what ();
        }
        _app.stop ();
    }

    void stop () noexcept override {}

    profile_res_t take_reply () const
    {
        if (_error) {
            throw std::runtime_error (*_error);
        }
        if (!_reply) {
            throw std::runtime_error ("transient trigger request produced no reply");
        }
        return *_reply;
    }

  private:
    zlink::framework::app_t &_app;
    profile_req_t _request;
    std::string _trace_label;
    std::optional<profile_res_t> _reply;
    std::optional<std::string> _error;
};

inline profile_res_t request_with_transient_host (const trigger_options_t &options,
                                                    const std::string &channel_endpoint,
                                                    const std::string &trace_label,
                                                    profile_req_t request)
{
    if (channel_endpoint.empty ()) {
        throw std::runtime_error ("trigger channel endpoint is required");
    }

    auto app = zlink::framework::app_t::create ();
    auto service = std::make_unique<transient_request_service_t> (app, std::move (request),
                                                                  trace_label);
    auto *service_result = service.get ();
    app.add_zlink_framework ([&] (zlink::framework::zlink_framework_options_t &framework) {
        framework.add_client_server_channel (profile_channel)
          .client ()
          .connect (channel_endpoint);
        if (!options.log_dir.empty ()) {
            framework.configure_dispatch ()
              .message_flow (zlink::framework::message_flow_log_mode_t::key_transitions)
              .trace_label (trace_label)
              .trace_log_file (options.log_dir + "/" + trace_label + "-flow.log");
        }
    });
    app.add_hosted_service (std::move (service));
    const auto exit_code = app.run (0, nullptr);
    if (exit_code != 0) {
        throw std::runtime_error ("transient trigger host failed");
    }
    return service_result->take_reply ();
}

inline void send_invalid_handshake (const std::string &endpoint)
{
    constexpr std::string_view scheme = "tcp://";
    if (endpoint.rfind (scheme, 0) != 0) {
        throw std::runtime_error ("handshake failure trigger requires a tcp endpoint");
    }
    const auto address = endpoint.substr (scheme.size ());
    const auto separator = address.rfind (':');
    if (separator == std::string::npos || separator == 0 || separator + 1 >= address.size ()) {
        throw std::runtime_error ("handshake failure trigger endpoint is invalid");
    }

    boost::asio::io_context io;
    boost::asio::ip::tcp::resolver resolver (io);
    boost::asio::ip::tcp::socket socket (io);
    boost::asio::connect (socket, resolver.resolve (address.substr (0, separator),
                                                    address.substr (separator + 1)));
    boost::asio::write (socket, boost::asio::buffer ("not-a-zlink-handshake"));
    boost::system::error_code ignored;
    socket.shutdown (boost::asio::ip::tcp::socket::shutdown_both, ignored);
}

} // namespace zlink::framework::e2e::runtime_monitoring::trigger
