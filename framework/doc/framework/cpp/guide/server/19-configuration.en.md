---
title: "19. Configuration · C++"
---

<!-- framework-adapter-nav:start -->
[Guide Home](../../../index.en.md) | [Previous: 18. DI Container](18-di-container.en.md) | [Next: 20. HTTP Hosting](20-http-hosting.ko.md)
<!-- framework-adapter-nav:end -->

# 19. Configuration

> **The document that owns this chapter's contract** — covered by
> [C++ configuration and host public contract](../../../common/spec/server/languages/cpp/interfaces/02-configuration-host.en.md).
> This chapter explains how to read config values from the CLI, environment variables, and
> JSON files. What you can configure is collected in [16. Options](16-options.en.md).

Covers how to read endpoints, ports, and behavior flags from the CLI/environment
variables/JSON files instead of hardcoding them. The entry point is `app.config()`
(`config_builder_t`).

## 1. Model: A Flat Key-Value Store

Every source merges into a single flat model. A key is a dot-(`.`)-separated path.

```text
sample.topology.apiHttpEndpoint = http://0.0.0.0:8080
sample.host.keepRunning         = true
environment.name                = production
```

## 2. Loading Sources

```cpp
auto &config = app.config ();
config.load_json ("config/match-api.json");            // Throws if missing
config.load_json ("config/local.json",                 // Passes even if missing
                  zlink::framework::optional_t::yes);
config.load_env ("MATCH_API__");                       // Prefix-matched env
config.load_cli (argc, argv);                           // --key=value
```

Key-mapping rules per source:

| Source | Input | Model key |
|------|------|---------|
| `load_json` | `{"sample":{"topology":{"apiEndpoint":"tcp://..."}}}` | Flattens nested objects: `sample.topology.apiEndpoint` |
| `load_env(prefix)` | `MATCH_API__sample__host__keepRunning=true` | Strips the prefix then `__` -> `.`: `sample.host.keepRunning` |
| `load_cli` | `--sample.host.keepRunning=true` | As-is: `sample.host.keepRunning` |
| `load_cli` (a flag with no value) | `--verbose` | `verbose=true` |

JSON string/number/boolean/null values are flattened to string values. The lookup side
parses them into the type it needs.

## 3. Priority

**A later-loaded source overrides the same key.** The conventional order is
"file < environment variable < CLI" -- CLI, loaded last, wins in the end.

```cpp
// Recommended order: base file -> per-environment file -> env -> cli
config.load_json ("config/match-api.json");
config.load_json ("config/match-api." + config.environment () + ".json",
                  zlink::framework::optional_t::yes);
config.load_env ("MATCH_API__");
config.load_cli (argc, argv);
```

When bootstrapping needs it (e.g., taking the file path itself via `--config=<path>`), read
the CLI once first, then override with CLI again after loading the file. The actual pattern
from the TicTacToe sample:

```cpp
app.config ().load_cli (argc, argv);                       // (1) Read --config
if (auto path = app.config ().model ().get ("config")) {
    app.config ().load_json (*path);                       // (2) Load the specified file
}
app.config ().load_env ("ZLINK_CPP_SAMPLE__").load_cli (argc, argv);   // (3) env < cli
```

## 4. Reading Values

### A Single Value

```cpp
auto endpoint = app.config ().model ().get ("sample.topology.apiHttpEndpoint");
// std::optional<std::string>

bool keep_running =
  app.config ().model ().get ("sample.host.keepRunning").value_or ("false") == "true";
```

### A Section: Reading A Prefix Group

```cpp
auto section = app.config ().section ("sample.topology");
auto api = section.get ("apiEndpoint");          // sample.topology.apiEndpoint
auto registry = section.require ("registryRouterEndpoint");
// require throws framework_exception_t(request_protocol_error) if missing
```

## 5. Type Binding: bind<T>

To receive a config group as a struct, implement `static T bind(const configuration_section_t&)`
(the `configuration_bindable` concept).

```cpp
struct topology_t
{
    std::string api_endpoint = "tcp://0.0.0.0:5555";
    std::string api_http_endpoint = "http://0.0.0.0:8080";

    static topology_t bind (const zlink::framework::configuration_section_t &section)
    {
        topology_t value;
        value.api_endpoint = section.get ("apiEndpoint").value_or (value.api_endpoint);
        value.api_http_endpoint =
          section.get ("apiHttpEndpoint").value_or (value.api_http_endpoint);
        return value;
    }
};
```

```cpp
// nullopt if the section is missing -- falls back to the default
auto topology = app.config ().bind<topology_t> ("sample.topology")
                  .value_or (topology_t{});

// When the section must exist
auto topology = app.config ().bind_required<topology_t> ("sample.topology");
```

Put the bound struct into DI as a singleton, and handlers can receive it via injection
([Chapter 18](18-di-container.en.md)).

```cpp
options.services ().add_singleton<topology_t> (std::make_unique<topology_t> (topology));
```

## 6. Environment Name

Use an environment name when you need to distinguish deployment environments.

```cpp
app.config ().use_environment ("staging");

if (app.config ().is_environment ("production")) { /* ... */ }
auto suffix = app.config ().environment ();     // "staging"
```

## 7. Recommended Pattern Summary

1. **Gather it into one bootstrap function** -- since `load_*` order is itself the
   priority policy, don't scatter it across the app (see the sample's
   `load_sample_configuration` pattern).
2. **Model topology as a `bind<T>` struct** -- rather than spreading string-key lookups
   across the codebase, bind it into a struct in one place and pass it through DI.
3. **Set defaults via struct member initialization** -- a `value_or` chain naturally
   becomes "override if configured."
4. Use `require`/`bind_required` for required values -- a missing config is better off
   failing loudly at boot time.

## 8. Related Documents

- The formal contract: [C++ configuration and host public contract](../../../common/spec/server/languages/cpp/interfaces/02-configuration-host.en.md)
- The list of configurable values: [16. Options](16-options.en.md)
- DI registration: [18. DI Container](18-di-container.en.md)
