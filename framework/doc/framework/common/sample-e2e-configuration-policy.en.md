# Sample/E2E Configuration Policy

This document defines the common rules for reading and delivering application configuration in
every framework language's samples and E2E. Since samples and E2E are execution examples users
reference, shell or PowerShell must not substitute for the application's configuration system.

This policy doesn't define a new public framework API contract. It fixes how each language's
framework host reads values through the formal configuration system it must adopt, and how
validated configuration is applied to the framework builder. The per-language criteria below aren't
a description that the current implementation is already complete everywhere — they're the
adoption goal samples and E2E must reach.

## 1. Scope

The following values all fall under this policy's application configuration.

- The endpoints the server and client use
- The Redis endpoint and key prefix
- The instance name and routing id
- Application request timeout and retry limits
- Log, trace, evidence, and business-state file paths
- TLS certificate and key file paths
- Codec, monitoring, and framework feature options

The runner can decide the per-run port, Redis endpoint, and temp directory. However, it must not
pass those decided values to the application as environment variables. It passes the framework host
a per-role configuration file path. A standalone client that isn't a framework host can receive the
endpoint it directly connects to, the request timeout, and the scenario selector as explicit CLI
options.

## 2. Required Rules

### 2.1 The Framework Host Uses A Configuration File

- Each server role receives a single configuration file containing only the values that role
  needs.
- The default configuration file lives in a `Configuration/`, `config/`, or resource directory
  matching that language's convention.
- If a dynamic port and a per-run Redis are needed, the runner generates a temporary per-role
  configuration file.
- The framework host executable's CLI only accepts `--config <path>` or the equivalent
  configuration-file-path option that language's host provides with the same meaning.
- Each server role uses a separate execution entry point. A single executable isn't switched
  between multiple server roles with `--role` or `--mode`.
- Endpoint, timeout, routing id, and the E2E scenario selector aren't passed as individual CLI
  options to the framework host.
- If the configuration file is missing or a required value is invalid, the process fails before
  starting the framework runtime.

### 2.2 Application Configuration Isn't Delivered Via Environment Variables

**Zero environment variables can be directly used by application code in samples and E2E.** The
configuration boundary of the server, client, handler, and application doesn't directly read an
environment variable, regardless of its purpose or name.

The sample/E2E runner, server, and client don't use environment variables for application
configuration. The pattern of the runner setting an environment variable and a child process
reading it is also forbidden.

The following direct accesses aren't used in sample/E2E application code.

| Language | Forbidden Direct Access |
|------|--------------------|
| Node.js | `process.env` |
| .NET | `Environment.GetEnvironmentVariable(...)` |
| Java/Kotlin | `System.getenv(...)`, `System.getProperty(...)` |
| C++ | `std::getenv(...)`, `getenv(...)`, `load_env(...)` for sample/E2E configuration |

Even if a per-language configuration system includes environment variables as a default provider,
it isn't applied to sample/E2E configuration. Don't configure environment variables to take
priority over values in the configuration file, and don't have the runner inject values through
that provider.

### 2.3 Only Validated Typed Configuration Is Used

- Configuration file parsing, default-value application, and required-value validation are done in
  one place — the `Configuration/` boundary.
- The server module, handler, store, and client scenario don't each re-read the configuration
  file.
- A component that uses configuration receives a completed configuration object via that
  language's DI or typed binding.
- The framework builder uses the validated configuration object as input. It doesn't re-query the
  global environment inside the builder or a module factory.
- Log paths, evidence paths, and the role name are also included in the same configuration object
  as the topology endpoint.

## 3. Per-Language Adoption Goal

| Language | Target Configuration Input And Binding | Framework Connection |
|------|---------------------|----------------|
| Node.js/NestJS | Reads the configuration file as a typed provider via `@nestjs/config`'s `ConfigModule`, validated at startup. | Injects the typed provider into `ZLinkModule.forRootFactory(...)`. |
| .NET/ASP.NET Core | Reads the configuration file as `IConfiguration`, binds it to an Options type, and validates. | Uses the validated Options in `AddZLinkFramework(...)` configuration. |
| Java/Kotlin/Spring Boot | Binds `application.yml` or `application.properties` to a `@ConfigurationProperties` type and validates. | `ZLinkFrameworkConfigurer` applies the bound configuration to the builder. |
| C++ framework host | Reads JSON with `app.config().load_json(...)` and converts with `bind<T>()` or `bind_required<T>()`. | Uses the binding result in the framework host configuration. |

Don't build a separate configuration-delivery helper or parallel configuration abstraction for just
one language. If there's a requirement that language host's formal configuration system can't
express, don't work around it in a sample or E2E — record it as a separate design gap.

## 4. Clients That Aren't A Framework Host

A Stream Connector client, HTTP client, and browser client may not be a framework host. In that
case, don't build a server-style framework module or a separate configuration system.

- A standalone client can receive the endpoint it directly connects to, the request timeout, and
  the E2E scenario selector as explicit CLI options. It validates required values, format, and
  range once at start and converts them into a typed client configuration object.
- If Redis, routing id, server role, the full topology, file paths, credentials, or framework
  options are needed, or a list or nested structure must be passed, use a client configuration
  file. A value that shouldn't be exposed on the CLI, like a secret, is also delivered via the
  configuration file. In that case, file reading and validation happen in only one place — the
  client entry point's configuration boundary.
- A browser client reads a static `config.json` or a `/config.json` the runner provides.
- Don't put endpoint constants, environment variable lookups, or the full server topology into a
  client scenario.
- The E2E scenario selector is an execution-control input, so it can be received via CLI.

## 5. The Boundary With The Runner And Tool Environment

This policy targets sample/E2E application configuration. It doesn't treat the existing
environment the OS and tools use to run a process as application configuration. For example, the
runner can inherit as-is the standard environment executable lookup, language runtime location, and
native library loader require. This is only the allowed scope for running the OS and tool
processes. The number of environment variables sample/E2E application code can directly read is
still zero.

However, don't create a new environment-variable interface owned by sample/E2E and use it as a
configuration bypass path. If a runner-owned choice is needed, like a Docker image or build
directory, manage it via a runner option or the repository's runner configuration. Manage the
readiness wait limit as runner configuration too, distinct from the application timeout. The
application process doesn't read this value.

## 6. Secrets And Temporary Configuration Files

- A certificate private key or credential isn't recorded in the repository's default configuration
  file.
- For E2E that needs a secret, the runner creates a per-run temporary secret file, or records an
  external secret file path in the configuration file.
- Restrict permissions on a configuration and secret file the runner creates so only the current
  user can read and write it. On POSIX, set the file mode to `0600`. On Windows, remove inherited
  ACLs and grant read/write only to the user running the runner. If permissions can't be
  restricted, don't start the process.
- On both normal completion and failure, the runner cleans up the temporary files it created.
- If configuration must be preserved for failure analysis, keep only a copy with secret values
  removed, and print the preserved path. Delete the original temporary file that contains the
  secret.

## 7. Runner Execution Order

An individual sample/E2E runner follows the order below.

1. Prepare the per-run directory and port.
2. Create any needed Redis container and confirm the real endpoint.
3. Generate each role's configuration file.
4. Pass the configuration file path to the role executable.
5. Confirm readiness, then pass the needed CLI options to the standalone client to run the
   self-check or E2E scenario.
6. Clean up the processes, Redis, and temporary configuration files it started.

The runner's configuration-related responsibility is limited to generating the configuration file
and passing the path. Configuration file parsing, required-value validation, per-server framework
builder configuration, configuration defaults, and the meaning of configuration items aren't
reimplemented in shell, PowerShell, or a common runner.

### 7.1 Runner Simplicity Rules

The sample/E2E runner is kept so a user can immediately understand the execution order. Simplicity
is judged not by file line count but by the responsibilities, conditional branches, duplicated
implementations, and bypass paths the runner carries.

- Shell and PowerShell scripts are kept as simple entry points that check arguments and call the
  common runner.
- The runner only handles execution preparation, generating the per-role configuration file,
  sequential server startup, readiness confirmation, client execution, result confirmation, and
  cleanup.
- Framework builder configuration, configuration defaults, and the meaning of configuration items
  aren't reimplemented in the runner.
- Don't add an environment-variable compatibility path, multi-stage fallback, or automatic
  discovery of an existing process.
- On an error, fail immediately and clean up only the processes, containers, and temp files that
  runner created for that run.
- Don't write the same execution logic redundantly, once each, in shell, PowerShell, and
  per-language scripts.
- Only split out into the common runner the execution behavior multiple samples or E2E actually
  share.
- Don't build one general-purpose runner that handles every sample's differences via conditional
  branches. Express per-sample differences as explicit input like a role list, execution command,
  and configuration file.
- The common runner only holds behavior reused with the same meaning, like starting a process,
  waiting for readiness, and cleanup. If an `if` or `switch` keyed on the sample name is needed,
  split that execution order out into a per-sample runner.
- Keep procedures a scenario actually needs, like a Redis container and multiple server roles.
  Automatic discovery, compatibility handling, and duplicate fallback that execution doesn't need
  are targets for simplification.

Keep the basic execution flow in the order: preparation, generating the per-role configuration
file, sequential server startup, readiness confirmation, running the client scenario, result
confirmation, cleanup. Don't add a separate step the scenario doesn't require.

## 8. Regression Checks

Each language's sample/E2E regression check confirms the following conditions.

- The runner doesn't pass endpoint, Redis, role, or log path via an environment variable or JVM
  system property.
- The application code doesn't directly read an environment variable.
- The per-language configuration system either doesn't register an environment-variable or JVM
  system-property provider, or explicitly removes it from the default provider list. Confirm the
  binding result doesn't change even if an external value with the same name as a configuration-file
  key exists.
- The framework host receives the configuration file path and binds it via that language's
  configuration system.
- The framework host doesn't receive or override configuration via a CLI option other than the
  configuration file path.
- The standalone client validates CLI input once at start and converts it into a typed
  configuration object.
- A missing required configuration or an invalid endpoint fails before the runtime starts.
- The configuration file the runner generates actually reflects the per-run port and Redis
  endpoint.
- The browser client reads `config.json` with no environment variable.

If an existing sample or E2E differs from this policy, the current behavior isn't accepted as an
exception — that item is recorded as a migration gap. It's judged complete once the framework host
switches to a configuration file and typed binding, and the standalone client switches to
validated CLI input or, where needed, a typed configuration file.
