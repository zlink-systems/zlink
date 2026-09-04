# C++ · Node 0.17.0 DONTWAIT campaign conformance review

## 결론

검토 대상 25개 commit 가운데 0.17.0 계약 적응은 3개, 기존 Framework 결함 수정은 5개,
원인 대신 증상을 완화한 변경은 2개, 현재 spec과 어긋나는 변경은 1개, runtime 의미가 없는
build/test/sample/refactor 변경은 14개다.

가장 위험한 항목은 `a22f8c880b`다. C++ Framework가 DONTWAIT routed request를 사용하면서
`ENOENT`를 retryable admission 부재로 취급하지만, 현재 Core D-B85 계약은 최초 unknown RID를
`NOT_CONNECTED+EHOSTUNREACH`로, 이미 발급된 wait token의 RID 제거 terminal만 `ENOENT`로
구분한다. 두 경우를 errno 하나로 합치면 permanent terminal을 deadline retry로 바꾼다.

검토는 각 hash의 `git show`와 `doc/plan/c016-worklog/**`의 hash 검색 결과 및 연결된 bucket
summary를 대조했다. 아래 line은 검토 시점의 `main` (`f7fb207fd3`) 기준이다. 보호 문서는 읽기만
했고 이 보고서 외 파일은 수정하지 않았다.

## Commit별 판정

| commit | 한 줄 요약 | class | 요구·위반 계약 | C++ / Node parity | verdict |
| --- | --- | --- | --- | --- | --- |
| `b32d4cae64` | C++ runtime을 pull-completion으로 전환 | **A** | `bindings/doc/spec/cpp/README.ko.md:657-669`의 exact dependency·public poller drain, `bindings/doc/spec/async-execution-model.ko.md:132-147`의 completion owner/submit race 계약 | C++의 async registry, reply-token optional, poller drain 전환이 계약을 직접 구현한다. Node도 `360181172f`에서 동등하게 전환했다. | **KEEP** |
| `4d263e66b9` | C++/Node DONTWAIT send를 binding async path로 통일하고 POLLOUT에서 WRITABLE drain | **A** | `core/doc/spec/core/05-polling.ko.md:298-308`, `core/doc/spec/core/socket/README.ko.md:930-970`: BACKPRESSURED wait token은 WRITABLE 뒤 caller가 resubmit하며 POLLOUT/POLLCOMPLETION은 drain 전까지 level이다. 언어 투영은 `bindings/doc/spec/{cpp,node}/README.ko.md:657-669,775-787` | C++ `framework/languages/cpp/framework/src/runtime/backend/raw_route_port.cpp:40-59,63-106`과 `raw_dealer_port.cpp:39-40` 및 stream/channel path가 POLLOUT·POLLCOMPLETION을 drain한다. Node `framework/languages/node/packages/framework/src/runtime/backend/node/node-socket-backend-adapter.ts:248-257`도 sync DONTWAIT 우회를 없애 동일 binding Promise terminal을 사용한다. | **KEEP** |
| `180323e6fa` | typed serializer가 정한 content type을 payload와 함께 운반 | **B** | `framework/doc/framework/common/spec/server/01-execution/05-payload-ownership-and-codec.ko.md:183-191`, `framework/doc/framework/common/spec/server/00-foundation/06-framework-api.ko.md:568-641`: declared type으로 선택한 serializer의 bytes와 wire content type이 한 쌍이어야 한다. OLD code는 type erasure 뒤 registry를 다시 조회해 다른 content type을 붙일 수 있었다. | C++는 현재 `framework/languages/cpp/framework/include/zlink/framework/contracts/codecs/serializer.hpp:207`의 `serialized_payload_t`와 channel call sites에서 두 값을 함께 전달한다. Node는 serializer 결과의 bytes/metadata를 객체로 유지해 같은 재조회 문제가 없어 별도 변경이 필요 없었다. 0.17의 completion timing과 무관하며 JSON 기본 serializer에서는 우연히 두 lookup이 같아 잠복했다. | **KEEP** |
| `0c15d3261f` | moved-from supply permit slot을 비워 다음 waiter가 재등록되게 함 | **B** | `framework/doc/framework/common/spec/server/00-foundation/06-framework-api.ko.md:163-167`, `framework/doc/framework/common/spec/server/01-execution/04-application-job-queue-and-backpressure.ko.md:131-135,395-402`: 가장 오래 기다린 live source로 permit을 직접 handoff하고 waiter 상태를 정확히 정리해야 한다. OLD code는 moved-from `std::optional`을 engaged로 남겨 다음 waiter 등록을 막았다. | C++의 `std::optional` move-state 고유 결함이다. Node의 Promise/queue waiter에는 “engaged지만 값은 이동됨” 상태가 없어 동등 patch가 필요 없다. 새 async completion interleaving이 빈도를 높였을 뿐 계약 변경은 아니다. | **KEEP** |
| `504d39fc6e` | C++ sample에 nested authenticate handler를 명시 등록 | **E** | runtime 계약 변경 없음. public sample registration을 명시한 fixture 변경이다. | Node sample도 언어별 registration 표면을 사용한다. runtime 구현 parity 대상이 아니다. | **KEEP** |
| `2f1de0b56d` | dead-target Actor join을 Unavailable로 매핑하고 ZoneWorld B8 proxy를 보정 | **B** | `framework/doc/framework/common/spec/server/03-spot-actor/08-routing.ko.md:322-331`과 `framework/doc/framework/common/spec/server/00-foundation/07-framework-error-model.ko.md:33-35,64-83`: resolve 뒤 exact owner/route를 잃은 현재 operation은 `Unavailable`이며 단순 deadline으로 바꾸지 않는다. `core/doc/spec/core/06-monitoring.ko.md:74,88,92-94`는 EDGE 없는 CONNECTION_READY를 count snapshot으로 정의한다. | C++는 exact target generation이 더는 admitted가 아닐 때 join의 `DeadlineExceeded`를 `Unavailable`로 바꾸고 edge 없는 READY를 신규 연결처럼 처리하지 않는다. Node도 `framework/languages/node/packages/framework/src/runtime/actors/actor-local-native-join.ts:640-668`, `framework/languages/node/packages/framework/src/runtime/foundation/raw-service-mesh-runtime.ts:965,1661`에서 같은 판정을 한다. 새 handover/timing이 dead-target 창을 재현했지만 오류 의미는 기존 Framework 계약이었다. B8 proxy 조각은 별도 E 성격이다. | **KEEP** |
| `af7afd28e7` | C++ crash-boundary를 .NET/Java/Node로 이식 | **B** | `framework/doc/framework/common/spec/server/03-spot-actor/08-routing.ko.md:322-331` 및 `framework/doc/framework/common/spec/server/00-foundation/07-framework-error-model.ko.md:33-35,64-83`의 `Unavailable` 경계 | Node의 exact generation admission 검사와 READY edge 검사까지 C++와 동등하다. 언어 구조만 다르고 terminal 의미는 같다. sample proxy 변경은 E지만 commit의 runtime 핵심을 B로 분류한다. | **KEEP** |
| `a22f8c880b` | routed submit의 NOT_FOUND+ENOENT를 retryable route absence로 분류 | **D** | 현재 `core/doc/spec/core/socket/07-router.ko.md:202-205,453-464`: DONTWAIT unknown RID는 `NOT_CONNECTED+EHOSTUNREACH`, token 없음; `ENOENT`는 `disconnect_rid()`가 기존 wait token을 끝낼 때의 terminal이다. `framework/languages/cpp/framework/src/runtime/backend/raw_route_port.cpp:147-168`는 `.async()` 즉 DONTWAIT다. `framework/languages/cpp/framework/src/runtime/backend/raw_binding_adapter.hpp:96-106`이 `ENOENT`를 transient로 넣은 것은 두 상태를 합친다. Framework `framework/doc/framework/common/spec/server/00-foundation/06-framework-api.ko.md:344-367`도 known route unavailable과 target snapshot 없음 및 deadline을 구별한다. | C++만 structured submit result를 잃고 errno-wide retry set에 의존한다. Node는 `framework/languages/node/packages/framework/src/runtime/channels/channel-transports.ts:1057-1060`, `framework/languages/node/packages/framework/src/runtime/backend/mesh-actor-session-node-adapter.ts:38-42`에서 `NotFound`와 `RouteNotConnected`를 구분하며 ENOENT 전체를 retryable로 만들지 않았다. `framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_m6b_runtime.cpp`의 기대는 `46ef4b0f03`에서 이미 `Unavailable`→`DeadlineExceeded`로 바뀌었고, `01e5c4613d`가 그 forensic comment만 지웠다. 구현에 맞춘 expectation 변경의 흔적이다. | **RE-FIX AT ROOT** — C++ binding result/adapter에서 D-B85의 두 terminal을 보존하고 M6B 기대를 의미 기준으로 복원 |
| `44b9b27efc` | 같은 endpoint의 replacement RID 전에 stale connect intent를 retire | **B** | `framework/doc/framework/common/spec/server/02-channel-transport/04-network-listener-identity.ko.md:295-306 §replacement`, `framework/doc/framework/common/spec/server/02-channel-transport/01-channel-topology.ko.md:467-487`: replacement는 같은 endpoint라도 새 lifecycle·UUID RID이며 이전 endpoint intent가 새 RID connect보다 먼저 끝나야 한다. | C++ `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp:787-825`는 old endpoint disconnect→새 `connect_routing_id`→connect 순서다. .NET `b28eb24270`의 `DisconnectTransport`는 last old endpoint intent일 때 endpoint disconnect하고, `ebff5b3e1b`은 이미 admitted된 same-RID pair 재사용을 보존하므로 같은 규칙을 다른 lifecycle 지점에서 구현한다. Node `framework/languages/node/packages/framework/src/runtime/foundation/raw-service-mesh-runtime.ts:247-270`은 아직 바로 `connectToRoutingId`하고 stale same-endpoint intent를 retire하지 않는다. Node가 gate를 통과한 것은 replacement same-endpoint/RID-change 조합을 밟지 않았기 때문이며 구조적 면제가 아니다. | **KEEP** — 단, Node parity 결함은 별도 root fix 필요 |
| `dd028150c9` | plain-hello rejection test가 DEALER public completion poller를 구동 | **E** | test-only. public poller가 sole drain owner라는 `async-execution-model.ko.md:132-147`을 관측한다. | 임의 resend/sleep을 제거하고 실제 completion progress 뒤 rejection을 검사하므로 관측을 약화하지 않고 오히려 계약에 맞춘다. | **KEEP** |
| `cee95ff462` | bound-session actor test를 tokenless mailbox 주입 대신 실제 transport로 전송 | **E** | test-only. 실제 request completion과 reply를 관측한다. | synthetic mailbox record보다 C++/Node production 경로 parity를 더 강하게 검증한다. expectation 완화 없음. | **KEEP** |
| `dbfcf7d6fe` | C++ connector test가 bind 전에 RAW receive mode 선택 | **E** | connector fixture/perf test 설정만 변경. runtime library 동작 없음. | 명시적 STREAM receive-mode 선행조건을 test가 지킨 것이며 Node runtime과 무관하다. | **KEEP** |
| `4573c09a2a` | package install consumer에 producer configuration 전달 | **E** | CMake package-consumer harness만 변경. | runtime effect 없음. | **KEEP** |
| `20b94c3457` | system lz4 링크 시 static archive를 package에 설치 | **E** | build/package completeness 변경. | runtime policy나 Node parity 없음. | **KEEP** |
| `eb756181a6` | source-relative protobuf output include dir 노출 | **E** | build include path 변경. | runtime effect 없음. | **KEEP** |
| `01e5c4613d` | campaign refactor pass | **E** | lambda 공통화, include/comment/test helper 정리이며 diff상 branch·result·assertion은 동일하다. | C++/Node 모두 실행 의미 변화가 없다. 다만 `a22f8c880b` 관련 과거 assertion 설명을 지워 provenance가 약해졌을 뿐 runtime 변경은 아니다. | **KEEP** |
| `360181172f` | Node runtime을 pull-completion model로 전환 | **A** | `bindings/doc/spec/node/README.ko.md:775-787`, `bindings/doc/spec/async-execution-model.ko.md:132-147`: submit Promise와 public poller가 binding completion을 단일 소유한다. | C++ `b32d4cae64`와 같은 상태 전환이다. 언어별 Promise/registry 표현만 다르다. | **KEEP** |
| `e86cf2cc8f` | diagnostics level validation을 Set membership으로 표현 | **E** | 허용 네 값과 rejection 결과가 전후 동일한 refactor다. | C++ 진단 level 의미에 영향 없고 runtime 분기 집합도 동일하다. | **KEEP** |
| `285cfa522a` | 첫 reply 유실 뒤 terminal replay를 위해 매 attempt에 남은 deadline 절반만 배정 | **C** | `framework/doc/framework/common/spec/server/03-spot-actor/04-actor-model.ko.md:662-667`은 동일 source lifecycle+OperationId의 terminal replay를 허용하지만 “절반” budget이나 모든 오류 재시도는 정의하지 않는다. `framework/doc/framework/common/spec/server/04-session/02-session-actor-binding.ko.md:626-643`은 stale owner operation을 다른 대상으로 자동 재전송하지 않는다. 구현 `framework/languages/node/packages/framework/src/runtime/foundation/service-stateful-runtime.ts:4008-4027`은 error 종류와 무관하게 half-budget retry한다. | C++ `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp:159-249`는 매 submit에 전체 remaining budget을 주고 `not_connected/route_unavailable`만 retry하며 `timed_out`은 terminal이다. Node test `framework/languages/node/test/m6b/m6b-user-spot-terminal-replay.contract.ts:68-123`은 첫 timeout을 인위적으로 만들 뿐 실제 two-process reply 유실 원인을 증명하지 않는다. 구조 차이가 아니라 Node mesh/binding reply-retirement 원인의 미확정 증상이다. | **RE-FIX AT ROOT** — 첫 reply의 flow/corr와 retired pair를 추적해 Node mesh 또는 binding/Core 소유층 수정; 임의 1/2 split 제거 |
| `2e3b1b47e4` | Node SupportChat idle 10 s, grace 2 s, client wait 20 s로 증가 | **C** | sample `framework/doc/framework/common/sample/supportchat/README.ko.md:354-365`는 준비/reconnect가 idle을 먹으면 일반 typed message로 기준을 갱신하고 control/sleep/budget 연장으로 대신하지 말라고 명시한다. | C++는 10 s/2 s지만 `framework/languages/cpp/samples/SupportChat/Client/supportchat_client_scenario.hpp:212-224,281-295,339-350`에서 keepalive와 arm-before-act를 한다. .NET/Java/Kotlin은 3 s/2 s 및 10 s bounded wait로 통과한다. 보존 flow는 재연결 후 첫 idle·resume push 도착과 두 번째 idle의 server 미발행을 보였다. 후속 `5268c30110`은 timeout을 늘리지 않고 typed keepalive와 모든 waiter 선등록으로 aggregate 3/3·전체 7 sample을 통과했다. 즉 push-after-resume 결함이 아니라 Node scenario ordering 결함이다. | **REVERT** — timing inflation과 그 값을 고정한 regex gate를 제거하고 `5268c30110`의 순서 수정은 유지 |
| `c67deb44e0` | ZoneWorld browser Vite entry asset 추가 | **E** | sample asset/build 입력만 추가. | runtime effect 없음. | **KEEP** |
| `2ceb137abe` | SupportChat lifecycle test가 SampleTimings를 사용하고 runner가 PASS marker 소유 | **E** | sample harness 중복 제거. | timeout 의미나 runtime delivery를 바꾸지 않는다. | **KEEP** |
| `4135d4edf6` | cross-language smoke가 spec message-flow attribute 이름을 사용 | **E** | `framework/doc/framework/common/spec/server/06-observability/03-message-flow-tracing.ko.md:144-162`의 `packet_name`, `flow_id`, `flow_origin`으로 fixture callback을 맞췄다. | C++/Node runtime instrumentation을 바꾸지 않고 smoke가 표준 이름을 읽게 한다. 관측 약화가 아니다. | **KEEP** |
| `8dd97bda2d` | optional boolean 부정을 `!== true`로 명시 | **E** | callback type은 `(name) => boolean`; 미등록 시 `!undefined`와 `undefined !== true`가 모두 true다. | lint-only, runtime truth table 동일. | **KEEP** |
| `ec2ee1ea50` | DeliveryDispatch offer deadline을 status publish 뒤 시작 | **E** | sample `framework/doc/framework/common/sample/deliverydispatch/README.ko.md:294-302`: courier가 offer를 받은 뒤의 acceptance deadline이어야 한다. OLD C++/.NET/Node는 선행 status publish 지연까지 budget에 포함했다. | Java가 이미 publish 뒤 deadline을 시작했다. C++/Node/.NET을 같은 sample 의미로 맞췄으며 Framework runtime 변경은 아니다. | **KEEP** |

## 재수정 권고

위험 순으로 다음 조치를 권고한다.

1. **C++ routed submit 결과를 D-B85에 맞게 다시 분리한다.**
   `raw_binding_adapter.hpp:96-106`의 errno-wide `ENOENT` transient 판정을 제거하기 전에 실제 설치된
   C++ binding package로 두 경우를 각각 고정하는 focused test가 필요하다.

   - DONTWAIT REQUEST, mandatory unknown RID: `NOT_CONNECTED+EHOSTUNREACH`, ID/token 0
   - BACKPRESSURED wait token 발급 뒤 `disconnect_rid`: WRITABLE terminal `ENOENT`

   그 뒤 `raw_route_port`가 structured submit result와 operation phase를 보존하도록 고치고,
   M6B public 기대는 “known route를 잃음=`Unavailable`, logical target snapshot 없음=`NotFound`,
   실제 reply deadline 만료=`DeadlineExceeded`”로 다시 고정해야 한다. 재시도 횟수 소진을
   `DeadlineExceeded`의 정의로 사용하면 안 된다.

2. **Node User Spot 첫 reply 유실을 flow 기준으로 재현한다.**
   `285cfa522a`의 half-budget은 느리지만 정상인 첫 attempt도 강제로 중단하고 모든 오류를 같은
   OperationId로 재제출한다. 첫 실패부터 동일 `flow_id`/correlation, target에서 operation terminal
   저장 시각, reply가 선택한 physical pair, HANDOVER 승자/패자 retire 시각, binding REQUEST terminal을
   한 타임라인으로 보존해야 한다. target이 실행·저장했는데 reply가 retired pair에서 사라지면 Node
   raw mesh admission/reciprocal collapse 또는 binding/Core reply routing을 고친다. target이 실행하지
   않았다면 admission 결과별 retry만 C++와 같이 제한한다. 원인 수정 전에는 1/2 상수를 계약으로
   승격하지 않는다.

3. **Node same-endpoint replacement parity를 보강한다.**
   `44b9b27efc`의 C++ 수정은 올바르지만 Node `connectPeer()`에는 동등한 stale endpoint-intent
   retirement가 없다. old RID와 new RID가 같은 endpoint를 쓰는 automatic replacement test를 만들고,
   old reconnect intent가 새 `connect_routing_id`를 덮지 못하게 해야 한다. stale generation의
   `disconnectPeer()`가 새 pair를 닫지 않는 현재 fence는 유지해야 한다.

4. **SupportChat은 순서 수정만 남기고 budget inflation을 되돌린다.**
   후속 `5268c30110`과 `bucketB-node-supportchat-aggregate-2-summary.md`가 root cause를 이미 확정했다.
   typed keepalive 뒤 reconnect, 각 action 전 waiter 등록, conversation-id filter를 유지하고 Node의
   idle/wait 값은 shared timing으로 복원한다. 보존된 실패에는 gateway가 버린 두 번째 push가 없으므로
   Framework push retry를 추가해서는 안 된다.

5. **Node 표준 gate의 false-green을 막는다.**
   `scripts/run_node_runtime_gate.js:47-52`의 file별 `--test-force-exit`가 등록된 뒤 아직 끝나지 않은
   tail test를 잘랐다. force-exit 대신 parent watchdog/명시적 handle cleanup을 사용하고, file별 TAP
   plan의 announced/completed count 불일치와 전체 예상 test count를 gate failure로 만들어야 한다.

## 스펙 gap 후보

1. **Infrastructure operation terminal replay의 sender algorithm.**
   Actor model은 terminal 보존과 같은 OperationId replay 허용만 정하고, sender가 어떤 transport
   terminal에서 재시도하는지, attempt timeout을 어떻게 end-to-end deadline에 배분하는지, 최대 attempt
   수가 무엇인지 정하지 않는다. C++의 “route 부재만 retry, timeout terminal”과 Node의 “모든 error,
   half-budget retry”가 모두 가능한 상태다. `285cfa522a`를 유지하려면 이 정책을 Framework spec에 먼저
   명시해야 한다.

2. **Framework의 routed admission error mapping 표.**
   public error model은 `NotFound`/`Unavailable`/`DeadlineExceeded` 의미를 정하지만 Core의
   `SUBMIT_NOT_FOUND`, `SUBMIT_NOT_CONNECTED`, initial `ENOENT`/`EHOSTUNREACH`, wait-token terminal
   `ENOENT`를 Framework terminal로 투영하는 operation-phase별 표가 없다. 이 공백이
   `a22f8c880b` 같은 errno-wide retry를 허용했다. Core D-B85 표를 Framework transport spec에
   참조시키고 SEND/REQUEST, NONE/DONTWAIT, initial/wait-terminal을 분리해야 한다.

3. **Core spec 이력과 campaign 전제 불일치.**
   `a22f8c880b` 시점의 router 문구는 missing RID를 일반적으로 `NOT_FOUND+ENOENT`라고 읽을 여지가
   있었고, 뒤의 D-B85가 NONE과 DONTWAIT 및 wait-token terminal을 분리했다. 현재 spec 자체는
   명확하지만 0.17.0 package/spec provenance에 D-B85 revision을 고정하지 않으면 소비자가 이전 문구를
   구현할 수 있다. release manifest에서 exact spec revision을 함께 고정할 필요가 있다.

`2e3b1b47e4`는 spec gap 후보가 아니다. SupportChat spec `§7.3`에 이미 typed message로 idle 기준을
갱신하고 budget 연장을 사용하지 말라는 규칙이 있으며 후속 순서 수정으로 재현이 사라졌다.

## Node test 수 1552 → 1536 조사

테스트 삭제나 rename이 아니다. 이전 `gate-v2-node-direct-tests.log`는 한 aggregate invocation에서
**1,557 result = 1,552 pass + 5 fail**을 냈다. 최종
`gate-final-unit-node-2-test.log`는 144개 파일을 하나씩 실행했고 **1,536 pass**만 냈다.
`contract-surface.test.js`는 마지막 test를 announce한 직후 `1..28`, `sample-regression.test.js`는
`1..39`로 끝났다. 현재 runner의 `--test-force-exit`가 두 파일의 tail 12개씩, 총 24개 등록 test를
완료 전에 종료한 결과다.

빠진 24개 test 이름은 다음과 같다.

1. `Spot public declarations use SpotId calls and keep Instance handlers actor-free`
2. `actor declarations resolve global ActorId calls and expose fluent manager shapes`
3. `dotnet to node channel stage uses the ClientServer transport contract`
4. `formal declarations expose role-specific ClientServer builders and exclude removed combined builders`
5. `framework aggregate runners never remove Redis containers or processes owned by another run`
6. `framework error kind values and exception surface match the shared table`
7. `framework public root excludes internal registration implementation`
8. `handler filter public contract exposes only the five supported dispatch kinds`
9. `location contract exposes only opaque provider primitives and aggregate operational queries`
10. `location wire enums retain values while provider write enums stay internal`
11. `node cross-language smoke covers bidirectional channel fanout route stream drain and store paths`
12. `node framework source tree does not keep emitted JavaScript beside TypeScript sources`
13. `node run_samples.sh executes every sample self-check`
14. `node sample wrappers delegate shared mechanics and sample-specific orchestration`
15. `node samples do not keep unreachable TypeScript files`
16. `node samples keep only contracts and shared sample configuration under Shared`
17. `node session samples do not implement sample-only actor session stores`
18. `node shared sample runner isolates Redis and application ports without Docker volumes`
19. `node to dotnet channel stage uses the ClientServer transport contract`
20. `node top-level sample runners execute every maintained sample`
21. `old public contract names from redesign rename table do not re-enter node surfaces`
22. `one-way call declarations complete without exposing transport admission results`
23. `route client surface scopes node routing by MeshName and resolves channels globally`
24. `stream connector and server HTTP one-way calls expose Promise<void>`

ZoneWorld dist 차이는 감소 원인이 아니다. 이전에는
`test/contract/sample-zoneworld-domain.test.js` 전체가 dist import 실패 한 건으로 기록됐고, 최종에는
그 파일의 실제 세 test가 통과해 순증가 `+2`다. `ec2ee1ea50`의 DeliveryDispatch test도 `+1`이다.
즉 현재 suite의 정상 완료 기대치는 기존 1,557에서 failed file placeholder `-1`, ZoneWorld 실제 test
`+3`, DeliveryDispatch `+1`을 반영한 **1,560**이며, 최종 1,536과의 정확한 차이 **24**가 위 잘린
test들이다.
