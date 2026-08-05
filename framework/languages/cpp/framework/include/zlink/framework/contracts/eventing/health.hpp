/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <memory>
#include <string>
#include <vector>

namespace zlink::framework
{

enum class health_status_t
{
    healthy = 0,
    degraded = 1,
    unhealthy = 2
};

namespace detail
{
class health_state_t;
} // namespace detail

enum class health_check_scope_t
{
    readiness = 0,
    liveness = 1,
    readiness_and_liveness = 2
};

struct health_check_result_t
{
    std::string name;
    std::string component;
    health_status_t status = health_status_t::healthy;
    health_check_scope_t scope = health_check_scope_t::readiness_and_liveness;
    std::string message;
};

struct health_report_t
{
    health_status_t status = health_status_t::healthy;
    health_status_t readiness = health_status_t::healthy;
    health_status_t liveness = health_status_t::healthy;
    std::vector<health_check_result_t> checks;

    bool ready () const noexcept { return readiness != health_status_t::unhealthy; }

    bool live () const noexcept { return liveness != health_status_t::unhealthy; }
};

class health_builder_t
{
  public:
    health_builder_t ();
    ~health_builder_t ();

    health_builder_t (health_builder_t &&) noexcept;
    health_builder_t &operator= (health_builder_t &&) noexcept;
    health_builder_t (const health_builder_t &) = delete;
    health_builder_t &operator= (const health_builder_t &) = delete;

    health_builder_t &add_zlink_runtime_check (std::string name = "zlink.runtime");
    health_builder_t &add_channel_check (std::string name);
    health_builder_t &add_location_check (std::string name);
    health_builder_t &add_stream_endpoint_check (std::string name);
    health_builder_t &add_hosted_service_check (std::string name);

    health_builder_t &
    set_status (std::string name, health_status_t status, std::string message = {});

    health_report_t report () const;

  private:
    explicit health_builder_t (std::shared_ptr<detail::health_state_t> state);

    health_builder_t &
    add_check (std::string component, std::string name, health_check_scope_t scope);

    std::shared_ptr<detail::health_state_t> _state;
};

} // namespace zlink::framework
