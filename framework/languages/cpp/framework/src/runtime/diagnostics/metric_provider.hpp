/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once
#if defined(_WIN32)
#include <winsock2.h>
#endif
#include <opentelemetry/metrics/noop.h>
namespace zlink::framework::runtime
{
using metric_provider_t = opentelemetry::nostd::shared_ptr<opentelemetry::metrics::MeterProvider>;
inline metric_provider_t noop_metric_provider ()
{
    return metric_provider_t (new opentelemetry::metrics::NoopMeterProvider);
}
} // namespace zlink::framework::runtime
