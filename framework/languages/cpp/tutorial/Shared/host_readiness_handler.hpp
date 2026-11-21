#pragma once

#include <zlink/framework.hpp>

namespace fw = zlink::framework;

class host_readiness_http_handler_t
{
  public:
    explicit host_readiness_http_handler_t (fw::framework_runtime_t &runtime) : _runtime (runtime)
    {
    }

    fw::http_response_t handle (const fw::http_request_t &)
    {
        return fw::http_response_t{_runtime.status ().is_ready ? 200 : 503, ""};
    }

  private:
    fw::framework_runtime_t &_runtime;
};
