# Stage 7 ZoneWorld 정찰: 정본 언어 선택 입력

조사일: 2026-08-23

## 판정 요약

**정본 후보는 .NET을 선택하는 것이 타당하다.** 네 구현 중 유일하게 runner가 ZW-A~G의
각 세부 판정을 클라이언트/서버 로그/runner 판정으로 분해하고, `zoneworld=completed`를
그 전부의 AND로 제한한다. Message Follow는 one-way와 request를 분리해 old-route relay와
target handler의 정확히 한 번 실행을 함께 확인한다. shared TypeScript browser도 runner의
`--browser-smoke` 경로로 연결되어 있다.

Node.js는 구현과 런타임 runner의 폭이 거의 같아 2순위이지만, ZoneWorld runner가 shared
browser를 실행하지 않고, `ZW-G1`은 canonical RID를 검증하지 않은 채 marker를 출력한다.
C++과 Java/Kotlin은 topology와 주요 application code는 존재하지만, 공통 완료 기준을
온전히 표현하는 client contract와 deterministic evidence가 없다. Kotlin은 Java framework를
공유하는 sample-only 포팅 대상으로 보아야 하며 정본 후보가 아니다.

이 문서는 **read-only 정적 조사**다. build/test/sample/E2E를 실행하지 않았으므로 아래의
“현재 실패”는 실행 결과 주장이 아니라, 현재 gate가 요구하는 조건과 정적으로 확인되는
미충족/미검증 경계를 뜻한다.

## 0. 현재 gate 편입 상태

요청의 “6-sample gate에서 제외”는 현재 checkout의 실행 목록에는 맞지 않는다.

| 언어 | 현재 aggregate / manifest | 결론 |
|---|---|---|
| Node | `samples/run_samples.{sh,ps1}`의 기본 7개 목록에 `ZoneWorld` | 편입됨 |
| .NET | `samples/run_samples.sh`의 `SAMPLES` 7개에 `ZoneWorld` | 편입됨 |
| C++ | `samples/run_samples.sh` 7개 및 CMake runner label `framework-sample-zoneworld` | 편입됨 |
| Java/Kotlin | `samples/sample-manifest.env`의 `JAVA_SAMPLES`, `KOTLIN_SAMPLES` 모두 `ZoneWorld` 포함 | 편입됨 |

JVM `samples/README.md`만 여전히 “same six sample scenarios” 및 6개 directory tree를 서술한다.
이는 gate exclusion의 근거가 아니라 stale documentation/inventory이다. 실제 `run_samples.sh`는
manifest를 순회하므로 현재는 ZoneWorld도 선택한다.

근거: `framework/languages/{node,dotnet,cpp}/samples/run_samples.sh`,
`framework/languages/java/samples/{sample-manifest.env,run_samples.sh,README.md}`,
`framework/languages/cpp/CMakeLists.txt:2453-2476,2514-2516`.

## 1. 언어별 inventory와 시나리오 상태

공통 canonical contract는 `framework/doc/framework/common/sample/zoneworld/README.ko.md`이다.
이는 Gateway + ZoneNode A/B + Ops + Redis Location/Relocation/Maintenance stores, shared browser,
headless self-check, browser flow, normal/crash replacement 및 cleanup을 요구한다(§1, §3-4,
§9-11).

| 언어 | 존재하는 sample/assets | contract/regression 및 실행 harness | 구현/증명 상태 |
|---|---|---|---|
| .NET | `samples/ZoneWorld/{Client,Shared,Server/{Gateway,ZoneNode,Ops}}`; shared browser는 `languages/shared_sample/zoneworld/client` | `run_sample.sh`(879 lines), `run_sample.ps1`, `ZoneWorldTopologyRegressionTests`, `ZoneWorldMaintenancePolicyTests`, `ZoneWorldOpsConsoleRegistryTests`, configuration regression | 가장 넓다. client `Scenarios.All`은 A1-A5/B1-B3/B5-B7/C1/C4/D1/E1-E4/E6/F1/F3/F4, runner-driven은 B4/C2/C3/E5/G2이고 runner가 F2/D1 server side/G1/G3/G4/G5를 합친다. Browser는 `--browser-smoke`일 때만 실행된다. |
| Node | `samples/ZoneWorld/{Client,Shared,Server/{Gateway,ZoneNode,Ops},Runner}` + same shared browser | `run_sample.sh` → generic `run-sample.mjs` → `Runner/sample-runner.mjs`; `sample-zoneworld-gate.test.js`, `sample-zoneworld-domain.test.js`, maintenance gate와 broad sample regression | A~G의 많은 client/special-client 흐름과 process disruption이 있다. A→B→A, old-route request/one-way, C4 timer fault, D2 extra subscriber, E5 restart, F bots, G3/G4 replacement를 runner가 orchestration한다. 그러나 runner에서 `ctx.runBrowser`/`startBrowser` 호출은 없고 `G1`은 실제 RID format을 검사하지 않는다. |
| Java | `samples/java/ZoneWorld/{Client,Shared,Server}` (Gateway/Zone/Ops code 분리) | own `run_sample.sh`(259 lines); `ZoneStatusReporterLifecycleTest` 한 개 | 4-role process/start/replacement wrapper와 application code는 있다. full client는 spawn/local move, OutOfRange/TooFar, A→B→A, node watch, announce, maintenance, diagnostics, lifecycle/replacement를 수행한다. 하지만 full runner가 명시적으로 요구하는 client verdict는 A2/B7뿐이고 Message Follow one-way, object/owner identity, border expiry, bot assertions, crash-Unavailable가 없다. |
| Kotlin | `samples/kotlin/ZoneWorld/{Client,Shared,Server}`; Java framework의 Kotlin facade sample | Java와 거의 같은 own `run_sample.sh`; `ZoneStatusReporterLifecycleTest` 한 개 | Java와 같은 coarse 3-mode client (`full/lifecycle/replacement`)와 topology가 있다. shared message set에는 Message Follow probe도 없고, Java보다도 relocation observability가 약하다. Kotlin-specific `await` connector surface를 보여 주는 sample 가치는 있으나 정본 가치가 없다. |
| C++ | `samples/ZoneWorld/{Client,Shared,Server/{Gateway,ZoneNode,Ops}}`, CMake executable targets | `run_sample.sh`(155 lines); `CppFrameworkSampleParity.CppZoneWorldRunnerRequiresContinuityEvidence`; CMake `framework-sample-zoneworld` | Entry/User Spot, actor relocation adapter, spot timers, border publish/subscribe, fanout, maintenance, bootstrap HTTP와 a single client exist. Client proves only C1, A3, A5, B2, B6, B7, D1; runner grep checks those plus bot/log markers. It does not model the complete ZW-A~G matrix, normal/crash replacement, lifecycle C2/C3/C4, E5 persistence, or browser. |

### Scenario-family coverage, by actual assertion rather than marker name

Legend: **P** = explicit protocol/process assertion; **S** = implementation exists but no complete
end-to-end assertion; **M** = missing/insufficient. A completion marker alone is not P.

| Family / required observable | .NET | Node | Java | Kotlin | C++ |
|---|---:|---:|---:|---:|---:|
| A: spawn, move, rejection order, sorted visibility | P | P | S (only A2 named) | S (only A2 named) | S (A3/A5 only) |
| B1/B4: adjacent-only border and expiry | P | P | M | M | M (border activity log only) |
| B2/B3/B5/B7: cross-node, local move, identity/generation, A→B→A binding | P | P | S (same player/session only) | S (same player/session only) | S (actor id + state only) |
| B6: stale old-route one-way + request Follow, payload/correlation/terminal failure | P | P | M (request/res messages exist; client does not drive it; no one-way type) | M (no probe message contract) | M (request only, not primed old route or one-way) |
| C: runtime status, shutdown/disconnect, spot-event timer failure | P | P | S (C2/C3 only; no C4 assertion) | S (C2/C3 only; no C4 assertion) | M (C1 only) |
| D: fanout all nodes/spots and unknown third subscriber | P | P | S (client announcement; replacement log only) | S | S (D1 client only) |
| E: targeted maintenance, admission, same-zone, persistence, diagnostics | P | P | S (toggle/diagnostic/persistence; not admission matrix) | S | S (toggle/diagnostic only) |
| F: 8 bots, cross-node bot relocation, no push, rejection reversal | P | P | S (bots implementation only) | S | M (bot logs only; no full client assertion) |
| G: generated RID, start order, normal and crash replacement | P | S (G3/G4 process choreography; G1 no RID assertion) | S (G3 normal replacement only) | S | M |
| shared real browser WS flow | P but opt-in | M | M | M | M |

Important static evidence:

- .NET’s final marker gate lists every individual verdict before emitting `zoneworld=completed`
  (`framework/languages/dotnet/samples/ZoneWorld/run_sample.sh:834-879`), while its client
  registry separates self-driven and runner-driven scenarios
  (`Client/Scenarios.cs:15-49`).
- Node’s main client explicitly probes before/after ObjectGeneration and executes both Follow
  forms (`framework/languages/node/samples/ZoneWorld/Client/main.ts:146-210`); the process
  runner checks exact one-way handler count (`Runner/sample-runner.mjs:84-104`).
- Java/Kotlin full client emits only `scenario ZW-A2 passed`, `scenario ZW-B7 passed`, then broad
  `zoneworld server evidence=completed` and `zoneworld=completed`
  (`.../Client/Program.{java,kt}:~99/~142/~206` and `~88/~130/~178`). The runner greps exactly
  those named scenario markers (`run_sample.sh:185-195`).
- C++ wire `ZoneChangedNotify` contains only playerId/zoneId and Follow defines only req/res;
  it has no owner/object-generation observables or one-way Follow message
  (`framework/languages/cpp/samples/ZoneWorld/Shared/Contracts/messages.hpp:37-40,68-69`).

## 2. Spec 17 (stage wrapper on Spot) coverage map

Spec 17 is a design/contract test target, not a requirement to introduce a framework Stage type.
The Zone Spot is the stage wrapper: it owns zone view/border/tick/admission; Player Actor owns
coordinate authority; handlers and timer callbacks must remain on framework Spot/Actor turns.

| Spec 17 requirement | Evidence in ZoneWorld | .NET | Node | Java | Kotlin | C++ |
|---|---|---|---|---|---|---|
| Public Spot composition only; no private RID/queue/timer exposure (§1-2, §9) | global Zone Spot factory, typed handlers and framework timers | meets statically | meets statically | meets statically | meets statically | meets statically |
| Zone state mutates on Spot turn (§3) | ZoneSpot handlers for join, move, border, announce and tick | meets structurally | meets structurally | meets structurally | meets structurally | meets structurally |
| Actor payload remains on Actor queue; actor changes stage via explicit Spot operation (§4) | Player Actor → join Spot / UpdatePosition; Spot sends state/push through actor | meets structurally | meets structurally | meets structurally | meets structurally | meets structurally, but compact single-file design is harder to audit |
| Spot lifecycle timer; no native scheduler/manual restore (§5) | zone tick 100 ms and bot tick 500 ms installed through context | meets statically; C4 real timer failure is observed | meets statically; C4 fault path exists | meets statically | meets statically | meets statically |
| Timer/Spot shutdown and relocation-seal behavior tested (§5, §9) | no timer callback after close; timer/pending tick restore boundary | no sample-specific direct test; framework/runner coverage is stronger | no direct lifecycle timer assertion beyond C4 | missing | missing | missing |
| Explicit create/GetOrCreate + membership admission (§6) | Zone bootstrap factory, OnActorJoin maintenance rejection | meets | meets | meets | meets | meets |
| Logical multicast is topic delivery, not durable membership (§6) | adjacent border topics only; diagonal excluded | P via B1/B4 | P via B1/B4 | S implementation only | S implementation only | S/log only |
| Location is global Spot ID; NodeId/RID not wrapper state/routing input (§7) | ZoneId routes; NodeId is maintenance label, RID evidence only | P (G1-G5) | S (static policy, weak G1) | S | S | S |
| Runtime metadata/operational observation separated from domain state (§8) | Ops runtime event + explicit reports | P | P | S | S | M |

All five trees have the structural stage-wrapper shape. None is enough to certify every Spec 17
test sentence solely from source. The important differentiation is observability: .NET and Node
exercise the edge boundaries; Java/Kotlin/C++ largely demonstrate the surface but do not prove
turn/lifecycle/relocation outcomes through the sample harness.

Spec source: `framework/doc/framework/common/spec/server/17-stage-wrapper-on-spot.ko.md:13-178`.

## 3. Relocation and maintenance scope (Specs 15/28/30 + common scenario)

ZoneWorld is meant to exercise these specific behavior classes, not generic “node restart”:

| Contract | Intended ZoneWorld observation | Current strongest coverage |
|---|---|---|
| Actor join triggers relocation when target Spot owner differs (15 §4.2; 28) | `MoveMsg` crosses edge; same ActorId/ObjectGeneration persists; owner generation changes; binding stays on same WS; A→B→A | .NET/Node P; JVM/C++ only same actor/player and post-move push, no full identity fence proof |
| PreserveStateWith stores application state only (15 §5) | coordinate, zone, bot direction/last movement are restored; framework queue/timer/membership/fence are not serialized by app | adapters exist in all five sample implementations; no cross-language payload fixture is currently asserted |
| Message Follow terminal semantics (15 §8; 28) | stale-route one-way and request preserve operation/payload/reply route; source does not re-resolve/retry | .NET and Node P; Java request/res declarations but no exercise; Kotlin absent; C++ request-only false-positive risk |
| Target-only authority commit / no crash failover (28) | target readiness precedes authority; Ready-owner crash ends current operation `Unavailable`, not new incarnation | Node disruptions and .NET runner strongly observe replacement identity; neither JVM nor C++ has canonical crash-Unavailable assertion |
| Host relocation/maintenance is a separate operation (30) | maintenance selected node state, normal stop/restart, fresh RID same NodeId; `SafeToShutdown`/drain semantics are runtime-owned | .NET has normal + crash replacement and log gates; Node has process variants; JVM normal replacement only; C++ absent |
| desired maintenance state + fanout; target admission decides (scenario §7.4) | only target NodeId changes, target cross-node arrival returns `ZoneMaintenance`, same-zone move allowed, restart restores desired state | .NET/Node P; Java/Kotlin toggle+persistence but incomplete admission matrix; C++ toggle/diagnostics only |
| border/bot maintenance pressure | snapshot only to adjacent topics, expiry at 3 ticks; bot has no binding and can relocate | .NET/Node P; other three mostly code/log presence |

Spec source: `15-spot-actor.ko.md:220-480,642-684,817-850`,
`28-relocation-flow.ko.md:`, `30-host-relocation-flow.ko.md:510-594,1038-1088`; scenario source:
`common/sample/zoneworld/README.ko.md:416-510,603-695`.

## 4. 정본 recommendation: .NET first

### Why .NET wins

1. **Completion semantics are non-forgeable within the runner.** The final marker is withheld
   unless every A/B/C/D/E/F/G verdict appears; Node prints analogous final markers after a
   procedural sequence but has less granular explicit G1 evidence. JVM and C++ emit broad
   markers after a subset.
2. **Observability is multi-plane.** The .NET runner combines client wire assertions, Flow/
   spot-discovery role logs, server-only evidence (all fanout subscribers/spots, bot absence,
   one-way Follow) and process lifecycle/RID evidence. It also has focused source regression
   tests for topology/maintenance/console state.
3. **Relocation test quality matches the hard contracts.** B6 proves request + one-way relay
   and exact handler counts; B7 asserts return to original owner with generation continuity;
   G3/G4 distinguish normal/crash replacement. This is the exact golden baseline the later
   ports need.
4. **Browser path already exists.** `--browser-smoke` builds and runs the shared client against
   only Gateway/Ops endpoints while the runner stops a node. It must become mandatory for
   reference certification, but the wiring is ready.

### Why not Node as the first reference

Node is a viable verification peer and its domain/contract tests are useful. It should run
against .NET goldens early. It loses because it currently lacks a ZoneWorld browser invocation
and its `ZW-G1 generated-routing-id=ready` is a printed statement, not an observed canonical
RID check. Use it as the first port, not the authority source.

### Why not JVM/C++

Java/Kotlin have large amounts of stage code but a coarse full client. Kotlin’s lack of a
Message Follow probe contract is a decisive blocker for a relocation-composite reference.
C++ has the most visible source/runtime risk: its client cannot represent identity/owner
generation and its runner declares broad completion from seven selected checks.

## 5. Golden extraction plan (from .NET only)

Freeze semantic values, not process-assigned RID/ports/timestamps. Each golden record should
have a scenario ID, normalized dynamic fields, producer/consumer role, and a link to the raw
role Flow/log artifact.

| Golden set | Contents / normalization | Port completion bar |
|---|---|---|
| Typed JSON wire vectors | canonical request/reply/push JSON for join, move rejection, ZoneState, ZoneChanged, border, Ops, maintenance, diagnostics, Follow req/res/msg. Preserve field name/null/optional semantics; replace UUID/RID/operation ID with tagged placeholders. | decoder/encoder exact match plus live client receives same semantic payload. |
| World-state transcript | ZW-A1-A5/B1/B3: spawn; fixed rejection precedence; UTF-8 order; adjacent-only snapshot; stale tick ignored/3-tick expiry. | same ordered transcript at specified steps. |
| Relocation transcript | B2/B5/B6/B7: before/after ActorId/ObjectGeneration/owner RID placeholder, binding session ID placeholder, source-old-route request + one-way, terminal missing-route error, A→B→A. | all correlations and generation predicates pass; no source re-resolve/retry evidence. |
| Stage/Flow event ledger | normalized Spec 26 flow/correlation events for join, target ready, restore, CAS/cutover, Follow relay, target dispatch, timer C4 failure; include outcome/error type. | expected ordered partial order and exactly-once handlers. |
| Store snapshots | Redis Location Store before/source/target authority states; Maintenance Store row before/after restart; Relocation Store rows ONLY from a scenario that per spec 28 §2 actually writes them (a pending request whose reply/terminal completes after cutover — Message Follow request class, e.g. ZW-B5/B7), plus a NEGATIVE assertion that a clean ZW-B2 relocation touches zero keys under the configured `relocation:` prefix (adjudicated 2026-08-24: spec 28 §2 restricts the Relocation Store to cold-activation creation info and post-relocation pending-request reply/terminal results — a clean B2 writing nothing is conformant, so absence is the golden, not a defect). Mask endpoint/RID/lease token but retain relationships/generation transitions. | same key namespaces/separation and state transition invariants, including the B2 relocation-prefix absence assertion. |
| Ops/fanout transcript | C1-C4/D1-D2/E1-E6: Registered vs Connected, node report/runtime event, one announcement per id, third subscriber, desired-state apply/persistence/admission. | identical observable states and duplicate/absence assertions. |
| Bot transcript | F1-F4: fixed eight bot identities/initial state, one correlated cross-owner relocation, no bound-session push, reversal after rejection. | normalized sequence and negative evidence. |
| Lifecycle transcript | G1-G5 + normal/crash replacement: canonical generated `zn-UUIDv4` shape, same NodeId/new RID, no Ready-owner automatic failover, separate crash fixture. | exact replacement and Unavailable boundaries. |
| Browser transcript | shared Playwright live test: only Gateway/Ops endpoints supplied, UI-visible join/move/ops/lifecycle behavior. | headless test and API/headless transcript both green. |

Extraction should archive raw `.flow`/role logs and Redis dumps as artifacts; the golden itself
should be a normalized, reviewable fixture plus a schema version. Do not make a log string the
only oracle: pair each log with typed wire/state assertions.

## 6. Concrete effort map to deterministic green

### .NET (reference completion work)

- Make `--browser-smoke` part of the reference gate, not an optional lane.
- Extract/commit the normalized goldens listed above, especially typed JSON and store/flow
  fixtures; today the runner proves local behavior but exports no portable golden package.
- Run the reference full process lane once after extraction and retain its exact artifacts.
- Add explicit reference assertions for any 28/30 handoff detail not exposed in the sample
  (target-ready-before-authority and `SafeToShutdown`/Message Follow window) rather than
  inferring it from a success marker.

### Node (first port)

- Consume .NET typed wire, relocation, store and transcript fixtures; compare all ZW IDs.
- Invoke the existing shared browser harness from `Runner/sample-runner.mjs` and require it.
- Replace G1 marker-only output with observed RID parsing/regex/distinctness; preserve G3/G4.
- Bind final completion to an explicit per-ID verdict set, as .NET does, and export normalized
  Flow/store artifacts rather than only checking log text.

### Java (second port)

- Split `full` into named ZW-A1..G5 scenarios; require every result before `zoneworld=completed`.
- Add `MessageFollowProbeMsg`, pre-relocation route priming, request + one-way exact-once target
  evidence, source relay evidence, missing-route terminal error, ActorId/ObjectGeneration/owner
  probe contract.
- Add B1/B4 border/expiry, C4 timer failure, D2 third subscriber, E admission matrix, all F
  bot negative/positive checks, G1/G4 crash/identity gates and shared browser lane.
- Replace runner’s A2/B7-only greps with a complete verdict manifest and add focused unit/contract
  tests beyond `ZoneStatusReporterLifecycleTest`.

### Kotlin (third port; sample-only facade)

- First converge its shared typed messages on the Java/.NET golden schema; it currently lacks
  Follow probe messages entirely.
- Port Java’s expanded scenario matrix using Kotlin connector `await`/`awaitReply`, not a
  second protocol/harness design.
- Reuse the Java runner/golden schema and add Kotlin-specific compile/API coverage only where
  the async facade differs.

### C++ (last port)

- Extend shared wire contracts with relocation identity/owner observations and one-way Follow
  probe; do not keep C++-only reduced declarations.
- Replace the monolithic client/grep runner with all named ZW scenarios and runner-only verdict
  ledger; assert B1/B4, B5/B6 full semantics, C2-C4, D2, E1-E6, F1-F4, G1-G5.
- Implement normal/crash replacement, desired-state restart and true `Unavailable` boundary;
  add shared browser execution.
- Add focused tests for ZoneState ordering/expiry, stage-timer shutdown, adapter payload
  compatibility and runtime Flow/relocation boundaries. The current C++ parity test only proves
  that the script contains expected strings.

## 7. Gate/failure interpretation and residual risk

No current execution result was produced in this recon. Thus this report does **not** call any
implementation “green” merely because source contains a test, nor “failing today” merely because
a historical run once failed. The immediate deterministic failures are the missing assertions and
contracts listed above: a port cannot satisfy the proposed golden-pass bar without them.

Historical notes, not current certification: an earlier C++ effort had focused runtime success
but still lacked logical-multicast/full-sample/ASan closure; an earlier JVM effort had successful
installDist but no completed four-role smoke. Re-run them only after the deterministic gates are
expanded; do not use those old outcomes to select the reference.

## Read-only evidence index

- Common sample contract: `framework/doc/framework/common/sample/zoneworld/README.ko.md:6-33,142-161,385-510,603-695`.
- Stage wrapper contract: `framework/doc/framework/common/spec/server/17-stage-wrapper-on-spot.ko.md:13-178`.
- .NET scenario registry/runner: `framework/languages/dotnet/samples/ZoneWorld/Client/Scenarios.cs:15-49`; `run_sample.sh:628-879`.
- Node implementation/runner/static gate: `framework/languages/node/samples/ZoneWorld/Client/main.ts:43-312`; `Runner/sample-runner.mjs:1-249`; `test/contract/sample-zoneworld-gate.test.js:12-212`.
- Java/Kotlin clients/runners: `framework/languages/java/samples/{java,kotlin}/ZoneWorld/Client/src/main/*/.../Program.{java,kt}`; both `run_sample.sh:185-259`.
- C++ reduced client contract/runner: `framework/languages/cpp/samples/ZoneWorld/{Client/main.cpp,Shared/Contracts/messages.hpp,run_sample.sh}`; static parity `tests/Zlink.Framework.ContractTests/test_cpp_framework_sample_parity.cpp:2055-2080`.
