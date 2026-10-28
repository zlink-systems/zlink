# C++ Framework package dependencies

Both installed static and shared Framework packages expose `opentelemetry-cpp::api` as a
public CMake dependency. Package configuration finds the OpenTelemetry C++ `api` component.
Applications select and link their SDK, readers and exporters separately; these components
are not Framework public dependencies. The Unreal connector package remains independent
of the server Framework and its OpenTelemetry dependency.

After configuring the application-owned SDK provider `sdk`, pass it before host startup:

```cpp
auto app = zlink::framework::app_t::create();
app.use_metric_provider(
    opentelemetry::nostd::shared_ptr<opentelemetry::metrics::MeterProvider>(sdk));
return app.run(argc, argv);
```

The [C++ README](../README.md) contains the complete provider injection example.
The provider implementation must use the same OpenTelemetry ABI namespace and compatible
API headers, C++ standard library configuration, compiler ABI and runtime library as the
Framework package. Windows applications and DLLs must use compatible CRT settings and keep
the provider implementation module loaded until callback removal and host destruction.
The Linux package consumer tests for this change use the installed vcpkg OpenTelemetry
1.28.0 API and SDK with ABI namespace v1. Compatibility with other versions or Windows CRT
combinations requires separate validation.
