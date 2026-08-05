/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include <zlink/framework/contracts/eventing/health.hpp>

#include <algorithm>
#include <utility>

namespace zlink::framework::detail
{

class health_state_t
{
  public:
    std::vector<health_check_result_t> checks;
};

} // namespace zlink::framework::detail

namespace
{

zlink::framework::health_status_t combine (zlink::framework::health_status_t current,
                                           zlink::framework::health_status_t next)
{
    if (current == zlink::framework::health_status_t::unhealthy
        || next == zlink::framework::health_status_t::unhealthy) {
        return zlink::framework::health_status_t::unhealthy;
    }
    if (current == zlink::framework::health_status_t::degraded
        || next == zlink::framework::health_status_t::degraded) {
        return zlink::framework::health_status_t::degraded;
    }
    return zlink::framework::health_status_t::healthy;
}

bool participates_in_readiness (zlink::framework::health_check_scope_t scope) noexcept
{
    return scope == zlink::framework::health_check_scope_t::readiness
           || scope == zlink::framework::health_check_scope_t::readiness_and_liveness;
}

bool participates_in_liveness (zlink::framework::health_check_scope_t scope) noexcept
{
    return scope == zlink::framework::health_check_scope_t::liveness
           || scope == zlink::framework::health_check_scope_t::readiness_and_liveness;
}

} // namespace

namespace zlink::framework
{

health_builder_t::health_builder_t () : _state (std::make_shared<detail::health_state_t> ())
{
}

health_builder_t::health_builder_t (std::shared_ptr<detail::health_state_t> state) :
    _state (std::move (state))
{
}

health_builder_t::~health_builder_t () = default;
health_builder_t::health_builder_t (health_builder_t &&) noexcept = default;
health_builder_t &health_builder_t::operator= (health_builder_t &&) noexcept = default;

health_builder_t &health_builder_t::add_zlink_runtime_check (std::string name)
{
    return add_check ("zlink", std::move (name), health_check_scope_t::readiness_and_liveness);
}

health_builder_t &health_builder_t::add_channel_check (std::string name)
{
    return add_check ("channel", std::move (name), health_check_scope_t::readiness);
}

health_builder_t &health_builder_t::add_location_check (std::string name)
{
    return add_check ("location", std::move (name), health_check_scope_t::readiness);
}

health_builder_t &health_builder_t::add_stream_endpoint_check (std::string name)
{
    return add_check ("stream", std::move (name), health_check_scope_t::readiness);
}

health_builder_t &health_builder_t::add_hosted_service_check (std::string name)
{
    return add_check ("hosted_service", std::move (name),
                      health_check_scope_t::readiness_and_liveness);
}

health_builder_t &
health_builder_t::set_status (std::string name, health_status_t status, std::string message)
{
    auto found =
      std::find_if (_state->checks.begin (), _state->checks.end (),
                    [&] (const health_check_result_t &check) { return check.name == name; });
    if (found == _state->checks.end ()) {
        _state->checks.push_back (
          health_check_result_t{std::move (name), "custom", status,
                                health_check_scope_t::readiness_and_liveness, std::move (message)});
        return *this;
    }

    found->status = status;
    found->message = std::move (message);
    return *this;
}

health_report_t health_builder_t::report () const
{
    health_report_t report;
    report.checks = _state->checks;
    for (const auto &check : report.checks) {
        report.status = combine (report.status, check.status);
        if (participates_in_readiness (check.scope)) {
            report.readiness = combine (report.readiness, check.status);
        }
        if (participates_in_liveness (check.scope)) {
            report.liveness = combine (report.liveness, check.status);
        }
    }
    return report;
}

health_builder_t &
health_builder_t::add_check (std::string component, std::string name, health_check_scope_t scope)
{
    auto found =
      std::find_if (_state->checks.begin (), _state->checks.end (),
                    [&] (const health_check_result_t &check) { return check.name == name; });
    if (found == _state->checks.end ()) {
        _state->checks.push_back (health_check_result_t{
          std::move (name), std::move (component), health_status_t::healthy, scope, {}});
    }
    return *this;
}

} // namespace zlink::framework
