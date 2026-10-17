#pragma once
#include "server/shared/config.hpp"
#include <zlink/framework.hpp>

namespace MyGame
{
namespace fx = zlink::framework;
// Connection callbacks. Add authentication and application packet handling here.
class Session final : public fx::packet_stream_session_t
{
  public:
    fx::task_t<void> on_connected (fx::stream_t &) override { co_return; }
    fx::task_t<void> on_disconnected (fx::stream_t &) override { co_return; }
    fx::task_t<void> on_error (fx::stream_t &, const fx::stream_error_t &error) override
    {
        throw std::runtime_error (std::string (error.message ()));
        co_return;
    }
    fx::task_t<void> on_packet (fx::stream_t &,
                                const fx::session_message_context_t &,
                                const zlink::message_t &) override
    {
        throw std::invalid_argument ("No application packets are registered yet.");
        co_return;
    }
};
}
