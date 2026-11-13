from pathlib import Path
import re
root=Path('/root/jobs/fx/cx/v21/s033/jcount')
source_sites=[]; target_sites=[]
text=(root/'profile2/exec/thread-sites.txt').read_text()
current=None
for line in text.splitlines():
    if line.startswith('[source]'): current=source_sites; continue
    if line.startswith('[target]'): current=target_sites; continue
    m=re.match(r'(\d+)\s+\d+\s+(.+?)\s+(systems\.zlink\S+)\.([^: ]+):(\S+)\s+(Framework-only|shared binding|benchmark)$',line)
    if m:
        samples,role,cls,method,ln,layer=m.groups()
        current.append((int(samples),role,cls,method,ln,layer))

def source_path(cls, method):
    top=cls.split('$')[0]
    rel=top.replace('.','/')+'.java'
    if cls.startswith('systems.zlink.framework.spring.'):
        return 'framework/languages/java/zlink-framework-spring-boot-starter/src/main/java/'+rel
    if cls.startswith('systems.zlink.framework.'):
        return 'framework/languages/java/zlink-framework-core/src/main/java/'+rel
    if cls.startswith(('systems.zlink.runtime.','systems.zlink.contracts.')):
        return 'bindings/java/src/main/java/'+rel
    if cls.startswith('systems.zlink.bench.withgrpc.shared.'):
        return 'framework/bench/grpc/java/shared/src/main/java/'+rel
    if cls.startswith('systems.zlink.bench.withgrpc.client.'):
        return 'framework/bench/grpc/java/client/src/main/java/'+rel
    if cls.startswith('systems.zlink.bench.withgrpc.frameworkserver.'):
        return 'framework/bench/grpc/java/zlink-framework-server/src/main/java/'+rel
    if cls.startswith('systems.zlink.bench.withgrpc.proto.'):
        return 'framework/bench/grpc/java/shared/build/generated/source/proto/main/java/'+rel
    return rel

def owner(cls, method, layer):
    if layer=='benchmark':
        return 'Benchmark harness: payload, metrics, or submit-loop work.'
    if layer=='shared binding':
        if 'Message' in cls: return 'Binding: allocate, copy, pool, or release message storage.'
        if 'CompletionOwner' in cls: return 'Binding: register and settle the physical completion.'
        if 'NativeRouterReceiveSupport' in cls: return 'Binding: receive routed frames from the native socket.'
        if 'NativeMessage' in cls or 'NativeSymbols' in cls: return 'Binding: access or release native message state.'
        if 'RoutingId' in cls: return 'Binding: compare routing identity.'
        return 'Binding: perform the shared socket or native-message operation.'
    if 'ZLinkChannelEnvelope' in cls:
        return 'Framework: construct, encode, or validate request/reply envelope fields.'
    if 'ZLinkJavaSocketReceivePoller' in cls:
        return 'Framework binding adapter: retain the receive owner and poller readiness.'
    if 'ZLinkJavaRawServicePort' in cls:
        return 'Framework binding adapter: submit a request, deliver its terminal, and release owned messages.'
    if 'ZLinkJavaRawMeshNode' in cls:
        return 'Framework: dispatch the service frame or observe its peer route.'
    if 'ZLinkServiceOperationRegistry' in cls:
        return 'Framework: register, select, expire, or dispatch the logical operation terminal.'
    if 'ZLinkServiceOperationIds' in cls:
        return 'Framework: assign the service-operation correlation identity.'
    if 'ZLinkServiceM6AWireCodec' in cls:
        return 'Framework: encode or decode the internal service wire frame.'
    if 'ZLinkFrameworkMultipartView' in cls:
        return 'Framework: own and release the decoded multipart view.'
    if 'ZLinkCompletionBridge' in cls:
        return 'Framework: forward request cancellation or completion to its caller future.'
    if 'ZLinkStateLane' in cls:
        return 'Framework: check, await, or grant the serialized state-owner turn.'
    if 'ZLinkChannelSocketRegistry' in cls:
        return 'Framework: select the node route and submit the socket request.'
    if 'ZLinkServiceLivenessRegistry' in cls:
        return 'Framework: update the service connection liveness state on its owner lane.'
    if 'ZLinkCodecRegistration' in cls:
        return 'Framework: select the serializer or decoder for the content type.'
    if 'ZLinkMeshApplicationDispatcher' in cls:
        return 'Framework: resolve the target namespace for application dispatch.'
    if 'ZLinkSpringHandlerFactory' in cls:
        return 'Framework: construct, initialize, or destroy the registered request handler.'
    if 'ZLinkApplicationJobContext' in cls:
        return 'Framework: enter the application job context and check its turn boundary.'
    if 'ZLinkHandlerDispatch' in cls:
        return 'Framework: select and start application handler dispatch.'
    if 'ZLinkHandlerActivator' in cls:
        return 'Framework: resolve a runtime service used by the handler.'
    if 'ZLinkMeshDrainCoordinator' in cls:
        return 'Framework: validate that the mesh is available for the operation.'
    if 'ZLinkSerialExecutionQueue' in cls:
        return 'Framework: enqueue or select the next accepted serial execution turn.'
    return 'Framework: perform the named service-runtime decision.'

def render_sites(rows):
    out=['| Thread role | Δ samples | Site | Layer | Decision / operation |','|---|---:|---|---|---|']
    for n,role,cls,method,ln,layer in rows:
        name=method
        site=f'<code>{cls}.{name}</code><br><code>{source_path(cls,method)}:{ln}</code>'
        out.append(f'| {role} | {n} | {site} | {layer} | {owner(cls,method,layer)} |')
    return '\n'.join(out)

body='''# Java Framework request path — per-request counts and sources (#1639)

## Scope and measurement protocol

- Source revision: `fb254a964c` (`origin/main` at the start); worktree remained detached and `git status` was clean. No product source was changed.
- Built the existing Java gRPC benchmark once under `flock -s /root/jobs/fx/cx/measure.lock`. The build succeeded (67 actionable Gradle tasks).
- Fresh baseline used the `jfix` Java `run_local.sh` harness copied under this job directory, with only its process pinning changed: source `taskset -c 0-7`, target `taskset -c 8-15`. The source and target affinities were checked on running JVMs.
- All cells used condition B, 30 s warmup, 5 s active, and 3 rounds, with each run under the exclusive measure lock. `MemAvailable` stayed above 12 GiB; the lowest observed value during the job was 16.9 GiB.
- Framework and binding each produced 18 valid result files. The runner accepted all request counts; no contaminated cell, request error, abandoned request, or target error was found.
- Round order was Framework runs followed by binding runs. Raw logs and JSON are under `raw/framework/` and `raw/binding/`.

## Fresh baseline

Throughput is completed requests/s. Ratio is the Framework median divided by the binding median.

| Pattern | Bytes | Framework rounds; median | Binding rounds; median | Ratio | ≥0.6 |
|---|---:|---|---|---:|:---:|
| request-backpressure | 1024 | 58,040.0; 58,721.6; 59,652.2 → **58,721.6** | 127,855.6; 122,347.4; 128,848.8 → **127,855.6** | **0.4593** | No |
| request-backpressure | 4096 | 53,926.0; 54,034.0; 53,266.4 → **53,926.0** | 89,898.2; 88,856.6; 86,775.6 → **88,856.6** | **0.6069** | Yes |
| request-serial | 1024 | 9,038.0; 8,879.0; 8,031.4 → **8,879.0** | 14,975.6; 15,033.8; 14,987.4 → **14,987.4** | **0.5924** | No |
| request-serial | 4096 | 8,152.6; 8,162.0; 8,016.4 → **8,152.6** | 13,862.6; 14,551.0; 13,704.4 → **13,862.6** | **0.5881** | No |
| send-saturation | 1024 | 108,729.2; 111,948.6; 107,033.6 → **108,729.2** | 309,399.8; 309,167.4; 311,079.0 → **309,399.8** | **0.3514** | No |
| send-saturation | 4096 | 104,567.8; 103,778.6; 105,453.8 → **104,567.8** | 170,457.8; 174,201.6; 169,694.2 → **170,457.8** | **0.6135** | Yes |

Four cells are below the 0.6 target: backpressure 1024, serial 1024/4096, and send-saturation 1024. The fresh backpressure 1024 ratio is 0.4593, below the earlier `jfix` same-day ratio of 0.535.

## Request-backpressure 1024 profile

Two separate focused Framework/binding runs were recorded; these profile runs are not the fresh-baseline rounds. For CPU and syscall measurements, the active window was 5 s; per-thread CPU snapshots bracketed the active request phase and the source-idle drain. The denominator is the active completed-request count. CPU therefore includes the post-active completion drain, whose measured length is reported in the runner JSON. The allocation counter samples bracket the active interval at 100 ms cadence; the end sample can include up to 100 ms after the active boundary.

| Implementation | Active completions | Throughput in profile run | Source CPU | Target CPU |
|---|---:|---:|---:|---:|
| Framework | 299,261 | 59,715.0/s | 18.25 CPU-s total; **60.98 µs/request** | 15.47 CPU-s total; **51.69 µs/request** |
| Binding | 540,332 | 108,064.6/s | 8.62 CPU-s total; **15.95 µs/request** | 12.74 CPU-s total; **23.58 µs/request** |

Per-thread CPU from `/proc/<pid>/task/<tid>/stat` (user + system CPU; grouped below only for worker pools):

| Implementation / process | Thread | CPU time | CPU µs / active request |
|---|---|---:|---:|
| Framework source | `ZLINKbg/IO/0` | 3.90 s | 13.03 |
| Framework source | `ZLINKbg/IO/2` | 3.77 s | 12.60 |
| Framework source | `zlink-jvm-raw-mesh-bench` | 3.44 s | 11.49 |
| Framework source | `ForkJoinPool.commonPool` workers, aggregate | 6.44 s | 21.52 |
| Framework source | Other JVM threads, aggregate | 0.70 s | 2.34 |
| Framework target | `ZLINKbg/IO/1` | 3.97 s | 13.27 |
| Framework target | `zlink-jvm-raw-mesh-bench` | 3.47 s | 11.60 |
| Framework target | `ZLINKbg/IO/0` | 2.94 s | 9.82 |
| Framework target | `ForkJoinPool-1` workers, aggregate | 4.50 s | 15.04 |
| Framework target | Other JVM threads, aggregate | 0.59 s | 1.97 |
| Binding source | `ZLINKbg/IO/0` | 4.61 s | 8.53 |
| Binding source | `ZLINKbg/IO/2` | 3.72 s | 6.88 |
| Binding source | Other JVM threads, aggregate | 0.29 s | 0.54 |
| Binding target | `ZLINKbg/IO/0` | 4.25 s | 7.87 |
| Binding target | `ZLINKbg/IO/1` | 4.18 s | 7.74 |
| Binding target | `bench-raw-request` | 3.69 s | 6.83 |
| Binding target | Other JVM threads, aggregate | 0.62 s | 1.15 |

Allocation bytes are from the JVM's `jdk.ThreadAllocationStatistics` cumulative per-thread allocation counter, sampled every 100 ms and differenced around the active interval. Allocation event counts are JFR `jdk.ObjectAllocationSample` samples; they are **sample counts, not exact allocated-object counts**.

| Implementation / process | Allocated bytes total | Allocated bytes / request | JFR allocation samples | Samples / request |
|---|---:|---:|---:|---:|
| Framework source | 2,663,084,440 | **8,933 B** | 5,129 | 0.01720 |
| Framework target | 2,429,656,920 | **8,150 B** | 2,940 | 0.00986 |
| Binding source | 3,715,831,136 | **5,925 B** | 1,885 | 0.00301 |
| Binding target | 2,843,654,160 | **4,534 B** | 1,350 | 0.00215 |

`jdk.JavaMonitorEnter` with zero threshold records contended Java intrinsic-monitor enters, not all uncontended monitor acquisitions or `ReentrantLock` operations. During the active window it recorded 1,355 source and 6 target events for Framework (0.004528 and 0.000020 events/request), and 0 for both binding processes. Their total recorded wait was 5.34 ms source and 0.082 ms target. The second profile configuration disabled `jdk.ThreadPark` after the first profile emitted 2.53 million park events in 38 s and measurably disturbed profiling throughput.

`perf stat` counted `raw_syscalls:sys_enter` during the exact 5 s active window, attached to each JVM process. The event reported 100% time enabled and no lost-count indication.

| Implementation / process | Syscalls | Syscalls / completed request |
|---|---:|---:|
| Framework source | 488,682 | **1.6330** |
| Framework target | 260,691 | **0.8711** |
| Binding source | 4,378,691 | **8.1037** |
| Binding target | 2,283,184 | **4.2255** |

JFR has no event for ordinary `ForkJoinPool.execute` work-item posts or continuation scheduling. The benchmark therefore does not provide an exact per-request thread-hop count; CPU samples, thread parks, and syscall counts are not used as substitutes for posted-work counts.

## Sampled call sites covering the Framework-only delta

The call-site comparison filters JFR `jdk.ExecutionSample` events to the 5 s active interval and compares the first `systems.zlink.*` Java frame at each sampled stack. The Framework/binding sample totals were 194/39 on the source process (delta 155) and 429/38 on the target process (delta 391). Sites below are ordered by positive Framework-minus-binding sample delta within each thread role; listed sites sum to at least 80% of the total sample delta (source 124/155; target 312/391). Counts are statistical CPU samples, not exact invocation counts. `Framework-only` marks Framework runtime sites; `shared binding` marks binding/package sites; `benchmark` marks benchmark harness code.

'''
body += '### Source process\n\n' + render_sites(source_sites) + '\n\n'
body += '### Target process\n\n' + render_sites(target_sites) + '\n\n'
body += '''## Artifacts and limitations

- Fresh baseline logs and per-run JSON: `raw/framework/`, `raw/binding/`.
- CPU snapshots, perf counters, JFR recordings, and profile results: `profile2/` and `profile3/`.
- The profile run's throughput is not substituted for baseline throughput. All per-TID CPU deltas are in `profile2/thread-cpu.csv`. Allocation sample counts do not provide exact object counts; exact thread hops/work-item posts are not exposed by the JVM's standard recording events.
- No product changes, tests, commits, branch changes, or GitHub operations were made. The source worktree remains clean and detached.

리팩토링 점검: 성능 4건, POSDDD 0건, 불필요 코드 0건 발견. 고친 항목: 없음. 넘긴 항목: 0.6 미달 4개 cell의 runtime 비용 분석 및 개선.
'''
(root/'report.md').write_text(body,encoding='utf-8')
print(f'wrote {root}/report.md with {len(source_sites)} source sites and {len(target_sites)} target sites')
