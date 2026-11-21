# etcd provider test infrastructure

Uses only the Node.js 22 standard library. Providers and Store management processes are implemented outside this directory. The harness tests functionality and failures on a single development machine; it does not verify host separation in production deployments.

## Cluster commands

```sh
node scripts/etcd/etcd-cluster.mjs up --run-dir scripts/etcd/.runs/example
node scripts/etcd/etcd-cluster.mjs status --run-dir scripts/etcd/.runs/example
node scripts/etcd/etcd-cluster.mjs kill --run-dir scripts/etcd/.runs/example --member 1
node scripts/etcd/etcd-cluster.mjs resume --run-dir scripts/etcd/.runs/example --member 1
node scripts/etcd/etcd-cluster.mjs pause --run-dir scripts/etcd/.runs/example --member 1
node scripts/etcd/etcd-cluster.mjs resume --run-dir scripts/etcd/.runs/example --member 1
node scripts/etcd/etcd-cluster.mjs partition --run-dir scripts/etcd/.runs/example --member 1
node scripts/etcd/etcd-cluster.mjs heal --run-dir scripts/etcd/.runs/example
node scripts/etcd/etcd-cluster.mjs compact --run-dir scripts/etcd/.runs/example --revision 10
node scripts/etcd/etcd-cluster.mjs down --run-dir scripts/etcd/.runs/example
```

`up` requires a new run directory. Members are numbered from 1, with three by default. `--members 5` and `--quota-bytes N` are supported. `resume` resumes a paused member or restarts a killed member with the same data directory. `heal` removes all peer partitions in this run.
`cluster.json` records the schema version, run ID, pinned version, initial cluster token, decimal cluster ID, client endpoints, and each member's name, ports, data directory, log and member ID. Docker also records container IDs; native mode records PIDs and process start times from `/proc`.

Linux and WSL use the official binaries. Archives are checked against the SHA-256 in `etcd-version.json` and cached in `scripts/etcd/.cache/`, outside the run directory. `tar` is required; Linux x64 and arm64 are supported. A checksum mismatch fails. Windows and macOS use Linux containers through Docker. Select a backend explicitly with `--backend docker` or `--backend native`. Docker pins the official image by digest and creates separate client and peer networks per run. `partition` disconnects only the peer network. Native mode uses a peer-only proxy that reads the source member ID from etcd Raft HTTP to cut connections in both directions, including existing connections. It does not affect client RPCs. This test tool targets the pinned etcd version.

Docker assigns client host ports; peers use only the Docker network. Restarting a killed Docker member can change its host port, so read the endpoints from `cluster.json` again. Native mode records the run ID in exclusive per-port locks under `.runs/.ports/`; `down` removes only its own locks. Ports already in use are skipped during selection. An external process can still take a selected port before etcd binds it. That bind failure reports the port conflict without retrying.

Readiness requires matching cluster IDs across all members and successful linearizable reads and Txns. Servers use the transaction, request-size and compaction minimums from spec section 9.2. Port conflicts and readiness failures return errors and clean up only this run's resources. `down` preserves member logs and data directories. Docker cleanup identifies resources by run label; native signals identify processes by PID and start time. `.cluster.lock` rejects concurrent commands for the same run. If forced termination leaves a stale lock, confirm that no command for the run is active before removing the lock and running `down`.

## RPC holds

```sh
node scripts/etcd/hold-proxy.mjs serve --target http://127.0.0.1:2379 --state-file scripts/etcd/.runs/proxy.json
node scripts/etcd/hold-proxy.mjs hold-downstream --control http://127.0.0.1:CONTROL_PORT
node scripts/etcd/hold-proxy.mjs hold-upstream --control http://127.0.0.1:CONTROL_PORT
node scripts/etcd/hold-proxy.mjs release --control http://127.0.0.1:CONTROL_PORT
node scripts/etcd/hold-proxy.mjs disconnect --control http://127.0.0.1:CONTROL_PORT
node scripts/etcd/hold-proxy.mjs status --control http://127.0.0.1:CONTROL_PORT
node scripts/etcd/hold-proxy.mjs stop --control http://127.0.0.1:CONTROL_PORT
```

`serve` prints the proxy endpoint, control URL and PID, and records them in a file. The OS assigns ports when `--listen PORT` and `--control PORT` are omitted. The control listener binds only to loopback. `hold-upstream` holds client->server bytes; `hold-downstream` holds server->client bytes. Both affect existing and new connections. `release` releases both directions; `disconnect` closes current connections. The action has taken effect when the control response returns. TCP backpressure holds bytes without accumulating them in JavaScript arrays. The proxy does not interpret gRPC or Txn contents.

## Raw KV

```sh
node scripts/etcd/raw-kv.mjs put --endpoint http://127.0.0.1:2379 --key-hex 006b --value-hex 00ff
node scripts/etcd/raw-kv.mjs range --endpoint http://127.0.0.1:2379 --key-hex 006b --revision 2
node scripts/etcd/raw-kv.mjs range --endpoint http://127.0.0.1:2379 --key-hex 00 --end-hex 01 --limit 1000
node scripts/etcd/raw-kv.mjs delete --endpoint http://127.0.0.1:2379 --key-hex 006b
node scripts/etcd/raw-kv.mjs txn --endpoint http://127.0.0.1:2379 --body-file scripts/etcd/.runs/txn.json
```

Keys and values are supplied as hex. Range prints the gateway response and a `bytes` array containing `keyHex` and `valueHex`. Revisions are passed as 64-bit decimal strings. `--end-hex` is the exclusive end for range and delete. Txn files use etcd v3 gateway JSON with base64 byte fields. Writing metadata and payload in separate Txns can create fixtures with mismatched `mod_revision` values. The tool preserves invalid storage formats as supplied. Gateway errors produce a failing exit status.

## Fixtures and validation

```sh
node scripts/etcd/generate-fixtures.mjs
node scripts/etcd/validate-fixtures.mjs
node framework/runtime/conformance/validate-runtime-conformance-fixtures.mjs
node --test scripts/etcd/self-test.mjs
node scripts/etcd/verify-harness.mjs --backend docker --log-dir PATH
node scripts/etcd/verify-harness.mjs --backend native --log-dir PATH
node scripts/etcd/check-conformance-coverage.mjs cpp.json dotnet.json jvm.json node.json
node scripts/etcd/check-conformance-coverage.mjs --language node node.json
```

Golden fixtures fix namespace and Framework key bytes, metadata, clock, versions and corrupt records. Existing store-record fixture preimages are reused as opaque key input without interpreting their meaning. The conformance inventory registers 25 items from etcd section 12 and 10 common provider items from SPI section 10. Four Redis-only items record their source sections and reasons for exclusion. The inventory JSON is maintained manually; the generator produces only golden bytes. The validator compares `{ file, section }` and the original bullets and requires exactly one registration for each bullet. Use `validate-fixtures.mjs --etcd-spec PATH --spi-spec PATH` to check spec copies. Valid stored time is 0..9,223,372,036,854 ms; larger values and invalid clock lengths are corruption vectors. Test adapters must start a real management process for cases that require one. The harness does not create or update the clock key automatically.

Per-language results use the following JSON format. Every case ID must appear exactly once with `passed`; missing, skipped, failed, duplicate and unknown IDs fail validation. The default check requires all four languages.

```json
{
  "language": "node",
  "cases": [
    {
      "id": "etcd.cross-language-record",
      "status": "passed",
      "pairs": [{ "writer": "node", "reader": "node", "status": "passed" }]
    }
  ]
}
```

`self-test.mjs` checks acceptance of synthetic passing results, rejection of missing and skipped results, byte holds and releases, and raw KV gateway handling. `verify-harness.mjs` starts two runs concurrently, checks kill, restart, pause, peer partition, heal, compaction and raw KV, then cleans up both runs. It saves command output, member logs, manifests and cleanup results in the log directory. `--runs 4 --isolation-only` checks only isolation and cleanup of four runs. `--runs 1` checks the lifecycle with one cluster. These checks are not provider conformance results.

## Shared conformance runner

Install the Node workspace dependencies and complete `npm run build` in `packages/framework-locations-etcd` before running the following commands.

```sh
node scripts/etcd/conformance-runner.mjs
node scripts/etcd/conformance-runner.mjs --allow-unverified
node scripts/etcd/check-conformance-coverage.mjs --language node --allow-unverified node-conformance.json
node --test scripts/etcd/conformance-self-test.mjs
```

The runner creates one Docker cluster, executes all cases and mutations sequentially, then cleans up. `conformance-cases.cjs` owns the judgments for 35 IDs. Missing registered functions and unknown IDs are checked before execution. `--cases ID,ID` limits the cases under investigation while preserving failure of full coverage. `--no-mutations` disables mutation execution. The default run also requires detection of six mutations per registered language.

Node dependencies are read from `framework/languages/node/node_modules`. Set `ZLINK_ETCD_NODE_MODULES` to another node_modules path when using a different installation. The runner passes that path through `NODE_PATH` to Node child processes. The baseline provider uses the package build output at `framework/languages/node/packages/framework-locations-etcd/dist`. Set `ZLINK_ETCD_PROVIDER_DIST` to use package build output from an isolated copy. If `index.js` or `admin.js` is missing, the runner reports the build command and fails before starting the cluster.

Mutation builds transform source with `ts.transpileModule` and copy the canonical protos. The six mutation-target cases run first against an unchanged control produced by the same transformation. All control cases must pass before mutations run; results are saved in `mutation-control-results.json`. The baseline dist, control and mutations run in separate processes. A mutation is `not-built` if its target syntax is absent.

`--adapter-config PATH` accepts a JSON object keyed by language ID. Values contain `command`, `args` and optional `cwd` and `env`. All writer x reader pairs are generated from the registered languages. The default configuration at this stage registers only Node, producing one 1x1 pair per cross-language case. The [adapter JSONL document](../../framework/runtime/conformance/location-store-adapter-protocol-v1.ko.md) owns the protocol and result format.

Build .NET and Java outside the runner first. Use .NET SDK 8 and JDK 25, running the following commands from the repository root.

```sh
dotnet build framework/languages/dotnet/tests/Zlink.Framework.LocationStore.ProviderAdapter/Zlink.Framework.LocationStore.ProviderAdapter.csproj -c Release -m:1
(cd framework/languages/java && ./gradlew --no-daemon --max-workers=1 -Pzlink.includeSamples=false :zlink-framework-locations-etcd:installProviderAdapterDist)
```

The following adapter configuration runs 3x3 pairs. Replace `ROOT` with the absolute repository path. The Node package dist and dependencies can be selected with the environment variables above.

```json
{
  "node": {
    "command": "node",
    "args": [
      "ROOT/framework/languages/node/test/contract/location-etcd-fixtures/provider-adapter.cjs"
    ]
  },
  "dotnet": {
    "command": "dotnet",
    "args": [
      "ROOT/framework/languages/dotnet/tests/Zlink.Framework.LocationStore.ProviderAdapter/bin/Release/net8.0/Zlink.Framework.LocationStore.ProviderAdapter.dll"
    ]
  },
  "jvm": {
    "command": "ROOT/framework/languages/java/zlink-framework-locations-etcd/build/install/provider-adapter/bin/location-store-provider-adapter",
    "env": { "JAVA_OPTS": "-Xmx256m" }
  }
}
```

```sh
node scripts/etcd/conformance-runner.mjs --adapter-config adapters.json --allow-unverified
node scripts/etcd/check-conformance-coverage.mjs --language node,dotnet,jvm --allow-unverified node-conformance.json dotnet-conformance.json jvm-conformance.json
```

The runner fails before creating a cluster if the .NET DLL or Java bin script is missing. Each of the two cross-language IDs requires nine pairs. Each pair uses a separate namespace; missing, duplicate, skipped and failed pairs fail coverage.

`build-managed-provider.cjs` copies .NET and Java provider source into isolated copies and builds the control and six variants. Copies and output are placed in OS temporary directories and deleted after execution. .NET uses generated protos and deployment DLL dependencies from the baseline Release build. Java uses generated protos from the distribution jars, replacing the original provider classes in jar copies. The baseline provider and each variant run in separate processes. All six target cases must pass for a language's control before its mutations run. Missing target syntax and build failures are `not-built`; only assertion failures count as `detected`. Results record `language`; build logs are preserved under `ZLINK_ETCD_TEST_LOG_DIR/mutation-build/<language>/<mutation or control>/`.

## C++ and 4x4 execution

On Linux, use Node.js 22, .NET SDK 8, JDK 25, CMake, Ninja, a C++20 compiler and a Docker daemon. Install Node workspace dependencies, the repository-selected Core and binding packages, and vcpkg dependencies first. Configure the C++ build directory as follows. `VCPKG_ROOT`, `VCPKG_INSTALLED_DIR` and `ZLINK_LOCAL_PACKAGE_ROOT` are absolute paths to the prepared dependencies. If `ZLINK_LOCAL_PACKAGE_ROOT` is a C++-only cache, create the .NET and Java local feed paths first. Empty feeds do not prevent dependencies from being resolved through public NuGet and Maven repositories.

```sh
mkdir -p "$ZLINK_LOCAL_PACKAGE_ROOT/nuget" "$ZLINK_LOCAL_PACKAGE_ROOT/maven"
cmake -S framework/languages/cpp -B /build/cpp -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/.github/workflows/framework-cpp-system-toolchain.cmake" \
  -DVCPKG_TARGET_TRIPLET=x64-linux-zlink-release \
  -DZLINK_FRAMEWORK_CPP_WITH_ETCD=ON \
  -DZLINK_FRAMEWORK_CPP_BUILD_TESTS=OFF \
  -DZLINK_FRAMEWORK_CPP_BUILD_SAMPLES=OFF \
  -DZLINK_FRAMEWORK_CPP_BUILD_CROSS_LANGUAGE=OFF \
  -DZLINK_FRAMEWORK_CPP_INSTALL_FRAMEWORK=OFF
CMAKE_BUILD_PARALLEL_LEVEL=1 \
  bash scripts/etcd/build-and-run-4x4.sh /build/cpp /build/conformance --allow-unverified
```

`build-and-run-4x4.sh` builds the four adapters, writes `/build/conformance/adapters.json`, then starts the shared runner. The first argument is a caller-configured static C++ build directory; the second is the output path for the .NET adapter, configuration and logs. Node and Java use their standard workspace build paths. Build output is saved as `*-build.log` and execution output as `conformance.log` in the log directory. Set `ZLINK_ETCD_TEST_LOG_DIR` to change the log location. Remaining arguments are passed to the shared runner. Without a host clock VM, `--allow-unverified` permits only the three existing unverified items. Each night, the one-worker C++ job in `nightly-stress.yml` runs this script with its own build directory and `--allow-unverified` on a GitHub-hosted runner, and uploads the log directory as `etcd-4x4-conformance-logs`.

The C++ target `zlink_location_store_provider_adapter` is created only with `WITH_ETCD=ON` and is not registered with ctest. For a separate adapter configuration, set `cpp.command` to the executable's absolute path and `cpp.env.ZLINK_ETCD_CPP_BUILD_DIR` to the absolute baseline CMake build directory. `build-cpp-provider.cjs` builds isolated provider source copies and adapters in separate directories. It reuses generated protobuf objects and common libraries from the baseline build; the control follows the same procedure. Target syntax mismatches and build failures are `not-built`; production provider code is unchanged.

Each of the two cross-language IDs executes 16 writer x reader pairs. The existing runner owns pair namespaces and judgment and coverage rules for the 35 IDs. Recheck saved results with the following command.

```sh
node scripts/etcd/check-conformance-coverage.mjs --allow-unverified \
  /build/conformance/logs/node-conformance.json /build/conformance/logs/dotnet-conformance.json \
  /build/conformance/logs/jvm-conformance.json /build/conformance/logs/cpp-conformance.json
```

Containers must be able to access the Docker socket and etcd host-published ports. If Docker Desktop host loopback ports are inaccessible directly from a container, configure a TCP forwarder from container loopback to `host.docker.internal`. The forwarder passes bytes without interpreting cases or oracles. C++ mutation builds run sequentially to limit memory use. Reuse cached output only when compiler, vcpkg revision and triplet, and Core and binding versions match. Do not rename and mix an existing `x64-linux` cache with `x64-linux-zlink-release`; explicitly configure `-DVCPKG_TARGET_TRIPLET=x64-linux` when validating an existing cache.

On Docker Desktop, keep Node dependencies and package build output on a Linux filesystem. Place a workspace copy on a Linux volume, or copy the same package build output and select it with `ZLINK_ETCD_PROVIDER_DIST` and `ZLINK_ETCD_NODE_MODULES`. The latter also requires the provider's companion Framework and stream-wire package output and dependencies.

When running inside Docker, the runner's absolute repository path must refer to the same bind-mount path on the host Docker daemon. Connect the Docker socket and host network, and provide the SDKs, JDK, Node and Docker CLI. Without a clock VM, the three existing host clock cases remain `unverified` for each language.

`grpc-proxy.mjs` forwards shared etcd proto KV and Auth unary RPCs to real etcd. The runner creates a proxy with `startProxy({target})` and selects a boundary with `arm({boundary, key|prefix})`. `entered` signals arrival at the boundary; `release()` resumes forwarding. `loseResponse()` drops that response; `disconnect()` closes the connection. Request and response protobuf bytes are forwarded unchanged; decoding is used only for selection and recording. The proxy uses the test cluster's plaintext HTTP/2 endpoint.

| Boundary | Hold point |
|---|---|
| `apply-before` | Before forwarding a Txn containing Put or Delete upstream |
| `txn-response` | Before forwarding an apply Txn response to the client |
| `metadata-response` | Before forwarding a Range response for the selected metadata key range |
| `cleanup-before` | Before forwarding a cleanup Txn containing Delete upstream |
| `clock-next` | After forwarding the selected clock Range response, before forwarding the next RPC for the same key |

Without a host clock VM, those three items are recorded as `unverified` with a reason. The default run fails; only `--allow-unverified` permits them. Unverified results do not count as passes. The Node test entry point runs with `ZLINK_ETCD_CONFORMANCE=1`; set `ZLINK_ETCD_ALLOW_UNVERIFIED=1` to permit unverified items. Snapshot validity checks wait a real five minutes on the same instance.

The Node-only `location-etcd-host-clock.test.js` applies a `Date.now` preloader only to provider and management CLI processes with `ZLINK_ETCD_INTEGRATION=1`. It checks live and expired reads, conditional writes, snapshot pages and the management time increment bound under day-scale clock changes. This auxiliary test is to be removed when the stage-three VM checks are provided; it does not convert the shared runner's unverified results into passes.

`ZLINK_ETCD_TEST_LOG_DIR` selects the directory for results, command output, manager logs, proxy observations and member logs. The default is `scripts/etcd/.conformance-logs/`. `unverified.json` lists unverified items; `cleanup.json` records remaining containers, networks, processes and temporary directories.

## Client capability results

Code and original GitHub Actions logs were checked on 2026-11-15. Client selection, versions, lockfiles and licenses are owned by [C++](../../framework/languages/cpp/extensions/framework-locations-etcd/README.en.md#etcd-client), [.NET](../../framework/languages/dotnet/src/Zlink.Framework.Locations.Etcd/README.en.md#etcd-client), [Java](../../framework/languages/java/zlink-framework-locations-etcd/README.en.md#etcd-client), [Node](../../framework/languages/node/packages/framework-locations-etcd/README.en.md#etcd-client) in their `etcd client` sections.

F, C, N, D, J and K identify the commands and original result sources below. Commands select the relevant tests from prepared builds; they were not executed during this documentation task. `None` means no test for that capability was found. `No execution evidence` means test code exists, but the inspected CI logs do not establish its execution result.

- **F** — [run 43650235394, job 132616840063](https://github.com/zlink-systems/zlink/actions/runs/43650235394/job/132616840063), `Run the etcd 4x4 cross-language conformance`: successful. Command: `bash scripts/etcd/build-and-run-4x4.sh framework/languages/cpp/build/linux-ninja-release "$RUNNER_TEMP/etcd-4x4" --allow-unverified`. Names in the table are IDs from the [shared case implementation](conformance-cases.cjs). The original job log prints only aggregate results. Individual F case passes are inferred from the successful execution and current case and coverage code; the per-case artifact was not inspected.
- **C** — `test_cpp_framework_locations_etcd` in the same run and job: ctest reports `Passed`. Focused command: `ctest --test-dir framework/languages/cpp/build/linux-ninja-release -R '^test_cpp_framework_locations_etcd$' --output-on-failure`. `EtcdProvider.*` and `EtcdTls.*` are [gtest names in that suite](../../framework/languages/cpp/extensions/framework-locations-etcd/tests/test_etcd.cpp). They belong to the successful suite, but individual gtest results are absent from the original log. This evidence excludes `EtcdLive`, which requires environment variables.
- **N** — [run 43650235394, job 132616839962](https://github.com/zlink-systems/zlink/actions/runs/43650235394/job/132616839962), `Run Node framework release gate`: successful. The original TAP log reports `ok` for the test names below. Select them from the Node workspace with `node --test test/contract/location-etcd-admin.test.js test/contract/location-etcd-client.test.js test/contract/location-etcd-provider.test.js`.
- **D** — [run 44695707953, job 136153120822](https://github.com/zlink-systems/zlink/actions/runs/44695707953/job/136153120822), `linux-x64`: `Zlink.Framework.Locations.Etcd.Tests.dll` `Passed!` on net8.0 and net10.0. This test checks that the channel takes no service config and uses the 16 MiB send and 8 MiB receive settings; it does not send boundary-sized messages.
- **J** — [run 44695703519, job 136151823591](https://github.com/zlink-systems/zlink/actions/runs/44695703519/job/136151823591), `linux-x64 / Java 25`: both `ZLinkEtcdTransportLimitTest` tests in the table `PASSED`.
- **K** — [run 44695712497, job 136151854029](https://github.com/zlink-systems/zlink/actions/runs/44695712497/job/136151854029), `linux-x64 / node22`: `ok` for both test names in the table.
- D, J and K are the Framework .NET, Java and Node runs of the `fix/etcd-sizes` branch (#4077 size-boundary tests) on 2026-11-20.

The nightly run's overall conclusion is `failure`. The C++ and Node test jobs succeeded; a separate sanitizer job failed. F uses a plaintext etcd harness. TLS and auth tests in C and N use fake gRPC servers; they are not results from a real etcd harness with security enabled.

| Capability | C++ | .NET | Java | Node |
|---|---|---|---|---|
| Txn Compare VALUE LESS | F: `etcd.expiry-before-apply` | F: `etcd.expiry-before-apply` | F: `etcd.expiry-before-apply` | F: `etcd.expiry-before-apply` |
| Txn Compare CREATE | F: `spi.atomic-conflict` | F: `spi.atomic-conflict` | F: `spi.atomic-conflict` | F: `spi.atomic-conflict` |
| Txn Compare MOD | F: `etcd.condition-change-before-apply` | F: `etcd.condition-change-before-apply` | F: `etcd.condition-change-before-apply` | F: `etcd.condition-change-before-apply` |
| Revision-pinned Range | F: `etcd.snapshot-stability` | F: `etcd.snapshot-stability` | F: `etcd.snapshot-stability` | F: `etcd.snapshot-stability` |
| Linearizable reads | F: `etcd.minority-isolation` | F: `etcd.minority-isolation` | F: `etcd.minority-isolation` | F: `etcd.minority-isolation` |
| Header cluster_id / revision | F: `etcd.cross-language-record`; `etcd.cross-language-version-condition` | F: `etcd.cross-language-record`; `etcd.cross-language-version-condition` | F: `etcd.cross-language-record`; `etcd.cross-language-version-condition` | F: `etcd.cross-language-record`; `etcd.cross-language-version-condition` |
| ErrCompacted | F: `etcd.compacted-snapshot` | F: `etcd.compacted-snapshot` | F: `etcd.compacted-snapshot` | F: `etcd.compacted-snapshot` |
| NOSPACE | F: `etcd.quota-exhaustion` | F: `etcd.quota-exhaustion` | F: `etcd.quota-exhaustion` | F: `etcd.quota-exhaustion` |
| Transport errors | F: `etcd.leader-loss-before-result` | F: `etcd.leader-loss-before-result` | F: `etcd.leader-loss-before-result` | F: `etcd.leader-loss-before-result` |
| Lost Txn reply: no hidden retry | F: `etcd.leader-loss-before-result`; `spi.unknown-commit-reconstruction` | F: `etcd.leader-loss-before-result`; `spi.unknown-commit-reconstruction` | F: `etcd.leader-loss-before-result`; `spi.unknown-commit-reconstruction` | F: `etcd.leader-loss-before-result`; `spi.unknown-commit-reconstruction` |
| `keys_only=true` | None | None | None | None |
| TLS | C: `EtcdTls.MutualTlsCustomCaAndIdentityHostNameAndTrustValidation` | No execution evidence: `EtcdTlsTests.Custom_trusted_CA_and_client_certificate_chain_complete_mutual_TLS`, `Untrusted_server_certificate_fails_before_the_RPC_is_received` | No execution evidence: `ZLinkEtcdTlsTest.customCaAndClientIdentityEstablishMutualTlsAndUntrustedCaFails` | N: `etcd TLS uses normalized HTTPS URLs and supports trusted CA plus client identity` |
| Auth | C: `EtcdProvider.AuthenticationRefreshesOnlyOnceForRecognizedDescriptions`; `EtcdProvider.AuthenticationAndAllWriteCallsShareAbsoluteDeadline` | No execution evidence: `Authentication_rejection_reauthenticates_once_with_the_same_deadline`, `A_second_authentication_rejection_is_not_retried` | No execution evidence: `authenticationErrorReauthenticatesAndResendsOnlyOnce`, `authenticationAndAllWriteRpcsShareOneAbsoluteDeadline` | N: `only a rejected authentication RPC is replayed once, within the original deadline` |
| 16 MiB send boundary | C: `EtcdProvider.MaximumSendLimitUsesStoreFailureBeforeServerDispatch` | D: `EtcdTransportLimitTests.Channels_take_no_service_config_and_use_the_spec_bounds` (channel settings) | J: `ZLinkEtcdTransportLimitTest.sendLimitAcceptsExactly16MiBAndFailsOneByteMoreBeforeDispatch` | K: `location-etcd-client.test.js` 'etcd send limit accepts exactly 16 MiB and fails one byte more before dispatch' |
| 8 MiB receive boundary | C: `EtcdProvider.ReceiveLimitAcceptsFiveMiBAndRejectsNineMiBAuthenticationResponses` | D: `EtcdTransportLimitTests.Channels_take_no_service_config_and_use_the_spec_bounds` (channel settings) | J: `ZLinkEtcdTransportLimitTest.receiveLimitAcceptsExactly8MiBAndFailsOneByteMore` | K: `location-etcd-client.test.js` 'etcd receive limit accepts exactly 8 MiB and fails one byte more' |
| Cancellation | None (physical RPC cancellation); C test `EtcdProvider.ConsumerWaitCancellationDoesNotCancelOrRetirePhysicalOperation` checks that the RPC survives wait cancellation. | F: `spi.unknown-commit-reconstruction` | F: `spi.unknown-commit-reconstruction` | N: `etcd cancellation cancels a pending RPC; dispose drains and rejects new operations`; F: `spi.unknown-commit-reconstruction` |

Focused unit-test commands for .NET and Java are `dotnet test framework/languages/dotnet/tests/Zlink.Framework.Locations.Etcd.Tests/Zlink.Framework.Locations.Etcd.Tests.csproj` and `./gradlew :zlink-framework-locations-etcd:test` from the Java workspace, respectively. The inspected nightly .NET job runs other test projects, and its Java job runs core, integration and Spring starter tasks; neither establishes provider unit-test results. Test code is in [.NET provider](../../framework/languages/dotnet/tests/Zlink.Framework.Locations.Etcd.Tests/EtcdProviderTests.cs), [.NET TLS](../../framework/languages/dotnet/tests/Zlink.Framework.Locations.Etcd.Tests/EtcdTlsTests.cs), [Java provider](../../framework/languages/java/zlink-framework-locations-etcd/src/test/java/systems/zlink/framework/locations/etcd/ZLinkEtcdProviderTest.java) and [Java TLS](../../framework/languages/java/zlink-framework-locations-etcd/src/test/java/systems/zlink/framework/locations/etcd/ZLinkEtcdTlsTest.java).

The F cancellation case checks abort after submission and unknown commit in .NET, Java and Node. C++ uses `proxy.disconnect()` at [conformance-cases.cjs:635](conformance-cases.cjs), so it does not establish RPC cancellation capability. Size settings and maximum SPI batch passes do not replace tests on both sides of client send and receive limits. C checks acceptance at 15 MiB and rejection at 17 MiB for sends, and acceptance at 5 MiB and rejection at 9 MiB for receives. These differ from measurements immediately below and above exactly 16 MiB and 8 MiB.

### Remaining work

- Item 3: add `keys_only=true` tests in all four languages, tests on both sides of send and receive limits in .NET, Java and Node, and direct RPC cancellation tests in C++. Record execution results for existing .NET and Java TLS and auth unit tests. Distinguish real TLS and auth-enabled etcd harness results from fake server results.
- Items 5 and 6: connect the pinned resolved graph to capability execution, and record .NET and Java lockfiles and transitive dependency license inventories for C++, .NET and Java. Evidence for disabling .NET retries and hedging requires configuration or contract evidence for the selected transport, separate from the observed absence of retransmission after a lost reply.
- C++ pinning: the etcd-enabled gRPC Conan lock entry, vcpkg baseline and actual resolved gRPC/protobuf versions are absent. Pinning is a separate decision; this task does not change build files or lockfiles.
- The shared runner's three host clock IDs (`etcd.provider-clock-independent`, `etcd.manager-wall-clock-jumps`, `spi.snapshot-clock`) remain `unverified`. The [runner](conformance-runner.mjs) blocks them before execution; a successful F step does not verify this scope.
