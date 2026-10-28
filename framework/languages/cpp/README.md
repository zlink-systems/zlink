# C++ Framework

Applications configure an OpenTelemetry C++ Metrics API `MeterProvider`, including its SDK,
reader and exporter, and pass it to the host before `run`:

```cpp
#include <zlink/framework.hpp>
#include <opentelemetry/sdk/metrics/meter_provider.h>

int main(int argc, char **argv)
{
    auto sdk = std::make_shared<opentelemetry::sdk::metrics::MeterProvider>();
    // Configure application-owned readers and exporters on sdk before startup.
    auto app = zlink::framework::app_t::create();
    app.use_metric_provider(
        opentelemetry::nostd::shared_ptr<opentelemetry::metrics::MeterProvider>(sdk));
    return app.run(argc, argv);
}
```

The host retains shared ownership and creates all Framework instruments from that provider
under meter `zlink.framework`. Static and shared packages use this same API. Framework does
not query the OpenTelemetry global provider. An omitted or empty pointer selects a no-op
provider. Calling `use_metric_provider` after `run` starts throws `std::logic_error` and leaves
the provider unchanged. Collection does not require a runtime status query or observation.
Applications keep the provider active until the host has stopped and its collection callbacks
have been removed; Framework does not flush or shut down the provider.

See [package dependencies](packaging/README.md) for the consumer build requirements.
