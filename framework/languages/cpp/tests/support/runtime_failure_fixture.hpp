/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once
#include <opentelemetry/metrics/provider.h>
#include "runtime/diagnostics/monitoring_runtime.hpp"
#include <zlink/framework/contracts/configuration/zlink_builder.hpp>
namespace zlink::framework::test
{
inline auto runtime_failure_monitoring ()
{
    auto monitoring = std::make_shared<detail::monitoring_runtime_state_t> ();
    monitoring->use_metric_provider (opentelemetry::metrics::Provider::GetMeterProvider ());
    monitoring->runtime_failures = std::make_shared<runtime::runtime_failure_collector_t> ();
    return monitoring;
}
inline auto runtime_failure_builder ()
{
    zlink_builder_t builder;
    detail::bind_zlink_monitoring (builder, runtime_failure_monitoring ());
    return builder;
}

template <class Options> auto runtime_failure_options (Options options)
{
    if constexpr (requires { options.metric_provider; })
        options.metric_provider = opentelemetry::metrics::Provider::GetMeterProvider ();
    options.runtime_failures = std::make_shared<runtime::runtime_failure_collector_t> ();
    return options;
}

template <class State, class... Args> auto runtime_failure_fixture (Args &&...args)
{
    auto state = std::make_shared<State> (std::forward<Args> (args)...);
    if constexpr (requires { state->spot_state; })
        state->spot_state->monitoring = runtime_failure_monitoring ();
    else
        state->monitoring = runtime_failure_monitoring ();
    return state;
}
}
