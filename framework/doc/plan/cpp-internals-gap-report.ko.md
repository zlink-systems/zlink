---
title: "C++ Framework 계약·내부 구현 Gap 리포트"
---

# C++ Framework 계약·내부 구현 Gap 리포트

- **작성일 / 재검토일**: 2026-08-07
- **공개 계약 기준**: `framework/doc/framework/common/spec/`와 C++ exact interface의 현재 working tree
- **내부 구조 기준**: `framework/doc/framework/common/internals/` 01–12의 현재 working tree
- **의사결정 기준**: 승인된 DEC-01–DEC-17의 결과가 반영된 정식 spec
- **구현 기준**: 전체 audit 기준은 `425b9c2a8272`다. 이후 항목별 구현 checkpoint와 검증 결과는 각 행에 기록한다.
- **방법**: 정식 spec과 C++ exact interface를 먼저 public contract 기준으로 삼고, 12개 internals 문서의 **Decision** / **Result To Confirm**을 구현 구조 기준으로 사용했다. 해당 C++ public header와 runtime 경로를 직접 읽어 SATISFIED / GAP / PARTIAL / 코드만으로 확인 불가로 분류했다. 언어별 재량은 public 동작이나 관찰 결과가 달라지는 경우에만 gap으로 계상했다.
- **증거 경로 표기**: `common/` = `framework/doc/framework/common/`, `cpp/` = `framework/languages/cpp/framework/`, `core/` = `core/` 트리(프레임워크 외부).

Public API와 사용자에게 보이는 동작의 gap은 정식 spec과 exact interface만 근거로 판정한다. Internals는
그 계약을 구현하는 상태 표현, component 책임과 불변 조건의 차이를 판정하는 기준이며 새 공개 계약을
만들거나 spec을 덮어쓰지 않는다. Package·process 실행이 필요한 항목은 source gap과 섞지 않고 검증
증거 부족으로 별도 표시한다.

> **기존 기록과의 관계**: Public spec에 있던 구현 진행 기록은 삭제됐다. 이 보고서가 C++ open gap의 작업 기록을 소유하며, 승인된 결정을 반영한 정식 spec과 exact interface를 계약 기준으로 삼는다. Public spec에는 구현 상태나 이 plan 문서의 ID를 다시 넣지 않는다.

---

## 병렬 구현 세션 주의 사항

이 보고서는 다른 언어의 gap 작업과 동시에 진행할 수 있지만, 모든 작업은 현재 `main` checkout에서
수행한다. 별도 `git worktree`나 작업용 branch를 만들지 않으며, 작업 시작 시점의 `main` commit SHA를
작업 기록에 남긴다.

- 이 세션은 해당 언어의 production source·test·package 자료와 이 gap 문서만 수정한다. 다른 언어
  디렉터리, 다른 언어 gap 문서와 공통 spec·internals는 수정하지 않는다.
- `framework/runtime/protocol/`의 schema·generated 파일, cross-language fixture, 공통 검증 script처럼
  여러 언어가 함께 소비하는 파일은 통합 담당자 한 명만 수정한다. 변경이 필요하면 이 문서에 요구사항과
  예상 wire/API 영향을 기록하고 공용 선행 commit을 요청한다.
- 다른 세션의 변경을 원복하거나 포맷하지 않는다. Stage와 commit은 명시적인 경로 목록으로 제한하고
  `git add -A`를 사용하지 않는다.
- Gap 종결은 source 수정만으로 판단하지 않는다. Owner-layer regression, public API/exact snapshot,
  package 또는 clean-consumer, 관련 process E2E 증거를 각각 기록하고 통과한 항목만 종결한다.
- 언어별 작업이 `main`에 반영된 뒤 통합 담당자가 cross-language contract, service-wire fixture, 전체 문서
  검사와 process E2E를 다시 실행한다. 개별 성공을 전체 종결로 승격하지 않는다.

### 구현 중 리팩터링·checkpoint 규칙

Gap 하나 또는 서로 강하게 연결된 작은 작업 묶음의 동작과 회귀 test가 통과하면 다음 Gap으로 넘어가기
전에 리팩터링 checkpoint를 둔다. 마지막에 한꺼번에 정리하지 않는다.

- Production code는 POSD 관점에서 deep module과 information hiding을 강화하고, 의미 없이 인자를 전달하는
  pass-through 계층, 호출 순서에 의존하는 temporal decomposition과 중복 helper를 제거한다. DDD 관점에서는
  lifecycle·ownership·state transition·terminal error invariant를 해당 domain owner가 책임하게 정리한다.
- 같은 checkpoint에서 unit test도 POSD/DDD 관점으로 리팩터링한다. 반복 setup은 의도를 드러내는 fixture나
  builder 안에 숨기고, test 이름과 helper는 domain 용어와 observable behavior를 표현하게 한다. Production
  내부 구조를 그대로 복제하거나 실행 순서와 private 구현에 결합된 test는 제거하거나 계약 중심으로 바꾼다.
- 리팩터링 뒤 dead code, 사용하지 않는 wrapper·alias·fixture·dependency를 제거하고, hot path의 불필요한
  allocation·copy와 lock·queue contention도 함께 점검한다. 동작 변경이 있으면 owner-layer regression을
  먼저 추가하고 관련 unit test를 다시 실행한다.
- 관련 test가 통과한 의미 있는 checkpoint마다 해당 언어 경로와 이 문서만 path-limited staging하여
  commit하고 `main`에 push한다. Commit에는 닫은 Gap ID와 실행한 test를 남기고, 검증되지 않은 변경이나
  다른 언어의 변경을 섞지 않는다. Push한 commit SHA와 gate 결과를 이 문서의 해당 항목에 기록한 뒤
  다음 작업으로 진행한다.

### 최종 종료 전 Codex Sol 검토 관문

최종 종료 판정 전에는 해당 언어의 모든 Gap·PARTIAL·public contract 항목을 대상으로 `Codex Sol`
review를 수행한다. 요약이나 focused test 통과 여부가 아니라 항목별 exact interface, 정식 spec,
production runtime, owner-layer regression, package/clean-consumer와 process evidence를 서로 대조해
다음 사항을 확인한다.

- 항목이 누락되지 않았는지, 완료로 표시한 구현이 실제 계약과 다른 부분이 없는지 확인한다.
- 누락·오판·부분 구현을 발견하면 해당 항목을 GAP 또는 BLOCKED로 되돌리고 owner-layer 수정과 회귀
  증거를 추가한 뒤 같은 Codex Sol review를 반복한다.
- review 대상, 사용한 Codex Sol 모델/effort, 기준 commit 또는 candidate manifest, 발견 사항, 수정
  commit, 재실행한 gate와 판정을 이 문서에 `file:line` 근거와 함께 기록한다. 단일 test, 문서 존재,
  source compile 또는 과거 결과만으로 항목을 clean 처리하지 않는다.

모든 계약·구현 항목이 위 review에서 누락 없이 구현되었다는 판정을 받은 뒤에만 2차 구조 review를
시작한다. 2차 review도 동일한 `Codex Sol`을 사용하며, 대상은 해당 언어의 Framework runtime
production source와 unit test다. 실행 순서는 먼저 production runtime 리팩터링과 회귀 검증을
완료한 뒤 unit test 리팩터링을 진행하는 것으로 고정한다. 다음 네 범주를 각각 검토하고 결과를 기록한다.

1. **성능 비용** — 불필요한 allocation·copy, payload 변환 왕복, lock/mutex/channel/atomic/queue
   contention, hot path의 중복 작업을 확인한다.
2. **불필요한 코드** — dead code와 도달하지 않는 branch, 사용하지 않는 wrapper·alias·helper·fixture·
   파일·dependency를 확인한다.
3. **POSD/DDD 구조** — deep module·information hiding, pass-through와 temporal decomposition,
   caller complexity, 중복 책임을 POSD 관점에서 확인하고 lifecycle·ownership·state transition·
   commit/deadline·terminal failure invariant의 domain owner가 명확한지 DDD 관점에서 확인한다.
4. **unit test 구조** — runtime 리팩터링으로 보존해야 할 observable behavior와 domain invariant를
   기준으로 test를 다시 읽는다. POSD/DDD 관점에서 동일한 의도·계약·fixture를 반복하는 중복 unit
   test는 하나의 명확한 test 또는 공통 parameterized/fixture test로 통합하고, 의미 없는 복제 test,
   private 구현·호출 순서에만 결합된 test는 회귀 증거를 보존한 뒤 제거한다. 통합·삭제 후에는 해당
   owner-layer regression과 aggregate gate를 다시 실행한다.

2차 review에서 Medium 이상 finding이 하나라도 남으면 clean으로 판정하지 않는다. 해당 runtime/test를
수정하고 필요한 owner-layer regression 및 관련 gate를 다시 실행한 뒤 같은 Codex Sol review를
반복한다. Low finding도 처리하거나 명시적으로 잔여 위험으로 승인 기록해야 한다. 두 단계의 review
결과가 모두 `CLEAN`, Medium 이상 `0`, 미실행 필수 gate `0`으로 기록된 경우에만 이 문서의 전체 작업을
완료로 판정한다.

## 1. 집계

| 문서 | GAP | PARTIAL | 비고 |
|---|---:|---:|---|
| 01-layering | 0 | 5 | relocation 재시도와 target propagation wait 종결 |
| 02-serialization | 1 | 3 | GAP 1건은 `CPP-EXEC-001` 락 횟수(2026-08-09 재확인: `spot_runtime.cpp:1353-1372` admission의 중첩 2락 — sealing gate의 queue 이관 또는 lock-free 재설계가 필요한 구조 항목). self-wait 오류 종류는 source 완료·E2E 대기 |
| 03-progress-isolation | 0 | 2 | owner HOL 블로킹과 executor 포화 process 종료 결함 종결 |
| 04-completion | 0 | 3 | 완료 방식은 domain owner별 terminal-once 경계를 사용하며 이동 오류 분류의 검증이 남음 |
| 05-relocation-continuity | 0 | 3 | 오류 종류 축약 |
| 06-routing-and-cache | 0 | 3 | 수동 피어 fence source 수정 뒤 process 검증 대기 |
| 07-dispatch-loop | 0 | 3 | timer history 상한과 overrun 계약 종결 |
| 08-object-lifecycle | 0 | 3 | generation 필터링과 Ready owner loss 판정 |
| 09-session-binding | 0 | 4 | command 36/38·51 구현과 callback/timer 경로는 있으나 전체 conformance matrix 검증 중 |
| 10-liveness-and-state | 0 | 1 | `CPP-OBS-002` 종결 |
| 11-message-ownership | 0 | 8 | 2026-08-09 재검증: "전반 미준수"는 stale — OWN-001/002/004/006/007/009는 현재 source 준수 확인(증거 대기), 잔존은 OWN-003(relocation 경계의 payload 이중 보관, 재구조화 필요)과 OWN-008(erased outbound 경로의 map find×2 — 해소에 public serializer 계약 변경 필요)뿐. `CPP-OWN-005` 종결 |
| 12-service-wire-protocol | 0 | 7 | 스토어 키 포맷, json-v1 프로파일, Base64 source 구현 완료·cross-language 검증 대기 |
| **internals 소계** | **1** | **45** | source 수정 완료 항목은 E2E·package 증거 부족이면 PARTIAL로 유지함 |
| C++ exact public interface | 0 | 0 | diagnostics 2건, object query, STREAM timeout, Client role 오류와 HTTP builder 종결 |
| **합계** | **1** | **45** | 상세 61행. 종결 15건은 GAP·PARTIAL 집계에서 제외 |

---

## 2. 우선 대응 항목 (심각도 상)

즉시 수정 계획이 필요한 항목. 프로세스 종료, 이중 소유, 크로스 런타임 상호운용 파괴, 복구 불가 상태에 해당한다.

| ID | 요약 | 문서 |
|---|---|---|
| CPP-SESS-001 | command 36/38 conformance 구현은 완료됐고 full generation·cross-node 검증이 남음 | 09 §3 |
| CPP-SESS-004 | command 51·callback·non-blocking 100 ms close 구현은 완료됐고 전체 lifecycle matrix 검증이 남음 | 09 §3 |
| CPP-WIRE-001 | Location Store 권한 키를 `zla1:…` 포맷으로 통합했으며 package와 cross-language Store 검증 진행 중 | 12 §1 |
| CPP-TOPO-001 | 수동 설정 피어 admission fence의 source 수정은 끝났고 stale manual peer negative process E2E가 남음 | 06 §1.1 |

### CPP-DISP-001 — executor 포화 → `std::terminate`
mesh 디스패치 스레드는 throwing `submit`으로 애플리케이션 작업을 넘기는데, `offload_executor_t::submit`은 내부 큐(4096) 포화 시 `std::runtime_error`를 던진다. 둘러싼 `catch (...)`는 정리 후 재던지고, pump 스레드 람다에는 try/catch가 없어 `std::thread` 본체를 탈출한 예외가 `std::terminate`를 호출한다. 서로 다른 owner의 in-flight 디스패치가 4096개를 넘는 순간 재현 가능하다. 스펙 03 §6은 "한도 초과를 조용히(또는 파괴적으로) 처리하지 말고 관찰 가능한 거부 결과를 내라"고 결정했다.
- 증거: `cpp/src/runtime/mesh/mesh_node_host_service.cpp:2706, 2804-2812, 2574-2836`, `cpp/src/runtime/dispatch/offload_executor.cpp:50-55`
- 구현 checkpoint `1cd08afc2d`: MeshNode pump와 local send가 throwing `submit`을 호출하지 않는다. Queue가 포화되면 원격 Request에는 `CapacityExceeded` envelope를 즉시 reply하고, local send에는 `backpressured`를 반환한다. Admission accounting과 mailbox claim도 같은 분기에서 terminal 처리한다. Executor saturation 회귀, `test_cpp_framework_target_contract`, `test_cpp_framework_host_lifecycle`는 통과했다. Idle-eviction fixture의 음수 timestamp를 수정한 checkpoint `af11dabeac` 이후 `test_cpp_framework_execution` 전체도 통과했다.
- process 종결 증거: `SubmitAdmission/run_e2e.sh CPP-DISP-001`이 격리된 C++ binding 0.10.1 candidate를 사용해 다섯 caller의 서로 다른 4,160개 ChannelName one-way dispatch로 실제 target pump의 application executor를 포화했다. 4,161번째 독립 ChannelName Request는 `CapacityExceeded`를 받았고, 포화 중 `/health`가 응답했다. Gate 해제 뒤 accepted handler가 drain됐으며 같은 public request 경로의 recovery도 완료됐다. 실행 log는 `framework/languages/cpp/e2e/SubmitAdmission/logs/20260808-033855-2618206/`이고 candidate native SHA-256은 `29a14581e19579fa6f2126ce1f9a797a08fe38d5788d52abb7e87689203f877b`다. 따라서 이 항목은 **CLOSED**다.

### CPP-DISP-002 — 노드 전체 head-of-line 블로킹
admitted 피어의 애플리케이션 레코드가 대상 owner의 mailbox 예산(1024건/64 MiB)에 들어가지 못하면 레코드를 `_pending_received` 단일 슬롯에 보관하고 `backpressured`를 반환하는데, 이 보관 레코드가 소진될 때까지 `pump_one`은 **노드 전체의 신규 애플리케이션 수신을 중단**한다. 결과적으로 (1) 포화된 Spot을 향한 원격 Request가 `Unavailable`/`CapacityExceeded`로 즉시 실패하지 않고 호출자 타임아웃까지 대기하고, (2) 느린 Spot 하나가 노드의 다른 모든 Spot 인바운드를 막는다. 스펙 03 §5는 Request 계열 즉시 실패를 요구하고, 수신 정지는 프로세스 전역 pending-byte HWM에만 허용한다.
- 증거: `cpp/src/runtime/mesh/raw_mesh_node_owner.cpp:1814-1829, 1984-1996`, `cpp/src/runtime/mesh/service_mailbox.cpp:63-94`
- 구현 checkpoint `bc748e9140`: application owner mailbox가 포화되면 record를 process-wide `_pending_received`에 보관하지 않는다. Request는 기존 reply 경로로 `workerQueueFull` terminal을 즉시 반환하고 one-way는 bounded drop으로 끝낸다. Infrastructure record의 bounded 보관은 유지한다. 한 node owner를 채운 뒤 같은 owner의 Request가 timeout 전에 실패하고 다른 ChannelName owner의 Request는 enqueue·reply되는 owner-layer 회귀를 추가했으며, 기존 one-way/liveness 회귀는 drop 뒤 payload가 다시 나타나지 않는 계약으로 갱신했다.
- public 오류·process checkpoint `bf412d9141`: raw request registry가 실패 reply header를 버리지 않고 public host까지 전달하며, Node·Channel request가 `workerQueueFull`을 `CapacityExceeded`로 복원한다. `test_cpp_framework_operation_registry`, `test_cpp_framework_messaging`, `test_cpp_framework_m6a_runtime`, `test_cpp_framework_m6b_runtime`, `test_cpp_framework_target_contract`, `test_cpp_framework_host_lifecycle`가 통과했다. `SubmitAdmission/run_e2e.sh CPP-DISP-002`는 격리된 C++ binding 0.10.1 candidate에서 slow ChannelName owner를 정지한 뒤 같은 owner의 Request가 `CapacityExceeded`로 끝나는 동안 독립 owner의 Request가 약 301 ms에 `Handled`로 완료되고 target health가 응답함을 확인했다. Gate 자동 해제 뒤 target handler `2/2` drain과 slow owner recovery도 완료됐다. 실행 log는 `framework/languages/cpp/e2e/SubmitAdmission/logs/20260808-040604-3267173/`이고 candidate native SHA-256은 `29a14581e19579fa6f2126ce1f9a797a08fe38d5788d52abb7e87689203f877b`다. 따라서 이 항목은 **CLOSED**다.

### CPP-SESS-001 — command 36/38 conformance 검증 중

정식 schema는 원격 bind를 `boundSessionBind(38)`의 correlation과 active/tombstone transition으로 표현하고,
bound-session send에는 `boundSessionSend(36)`의 expected binding generation을 요구한다. C++ codec은 두
record와 transition을 encode/decode하고, ingress에서 Actor owner·session identity·binding generation을 검증한다.
별도 JSON route packet이나 버려지는 send fence 경로는 제거됐다. `test_cpp_framework_service_wire_codec`,
`test_cpp_framework_target_contract`, 전체 focused CTest 7개와 SM-D6 process E2E가 통과했다. 같은 node와
cross-node 교체, stale·duplicate·pre-restart command, package consumer를 포함한 전체 36/38 conformance
matrix가 아직 남아 있어 **PARTIAL·검증 중**으로 둔다.
- 근거: `cpp/framework/src/runtime/protocol/service_wire_codec.hpp:320-370`,
  `cpp/framework/src/runtime/mesh/mesh_node_runtime.cpp:2479-2540`,
  `cpp/framework/src/runtime/stateful/public_host_runtime.cpp:2422-2485,5390-5431`,
  `framework/languages/cpp/e2e/SpotService/logs/20260808-172604-3982434/`.

### CPP-SESS-004 — 이전 session 교체 lifecycle 검증 중

승인된 계약은 새 exact identity를 current로 등록하면 bind를 즉시 성공시키고, 이전 owner의 ACK·callback·close를
기다리지 않는다. 따라서 현재 C++가 replacement route를 먼저 등록하고 응답하는 순서 자체는 문제가 아니다.
이전 session에는 `boundSessionReplaced(51)` one-way 통지를 보내고, `packet_stream_session_t::on_actor_binding_replaced(...)`
callback 뒤 non-blocking timer로 callback terminal 100 ms 뒤 close한다. Callback turn에서 `sleep`, blocking wait,
session lane·worker 점유를 사용하지 않으며, timeout이면 deadline에서 강제 close한다. 같은 physical session의
idempotent bind는 self-close하지 않고, retry·stale identity·다중 Actor cleanup은 exact retired identity로 제한한다.
SM-D6에서 replacement bind가 이전 callback·close를 기다리지 않고 반환되고 duplicate 안내와 새 session push가
확인됐으며, old session close는 callback 안내 뒤 100 ms 이전에 실행되지 않았다. 전체 lifecycle matrix와
cross-node/pre-restart retry 증거가 남아 있어 **PARTIAL·검증 중**으로 둔다.
- 근거: `cpp/framework/src/runtime/streams/stream_host_service.cpp:1036-1089,1693-1987`,
  `cpp/framework/include/zlink/framework/contracts/streams/stream.hpp:252-265`,
  `framework/languages/cpp/e2e/SpotService/logs/20260808-172604-3982434/`.

### CPP-WIRE-001 — 권한 키 codec strictness와 golden vector 검증 중
스키마가 고정한 `zla1:<a|s>:<byte-length>:<percent-encoded-id>`는 encoder와 decoder 모두 raw byte 길이 1..255,
leading zero 금지, literal unreserved 문자와 uppercase percent escape를 적용해야 한다. C++ encoder는 네 언어와
유효 입력의 byte 결과가 같고, `framework/runtime/protocol/golden/authority-key-v1.json`을 직접 읽는 C++ 회귀가
추가됐다. Decoder는 leading zero·비정규 escape·잘못된 UTF-8·길이 0/256을 거부하도록 보강됐으며 authority-key
focused test와 target contract가 통과했다. 설치 package와 다른 언어 provider를 함께 사용하는 process 검증이
남아 있어 **PARTIAL·검증 중**으로 둔다.
- 근거: `cpp/framework/src/runtime/locations/authority_key_codec.hpp:40-120`,
  `framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_authority_key_codec.cpp`,
  `framework/runtime/protocol/golden/authority-key-v1.json`.

### CPP-RELOC-001 — relocation 영구 차단
초기 audit에서는 `run_shared_relocation`의 `complete()`가 `blocked/target_unavailable`을 포함한 모든 결과에 `operation.terminal = true`를 설정하고, `relocation_operation`을 다시 시작 가능한 상태로 되돌리지 않았다. 이후 `relocate()` 호출은 저장된 blocked 결과를 계속 반환했다. 스펙 01 §3은 "거부된 결과는 저장하지 않으며, 재요청 시 처음부터 다시 검사한다"고 결정한다. 연관 gap으로 preflight와 worker가 대상 조회를 한 번만 수행해, target descriptor와 peer admission 전파 중 relocation이 즉시 거부됐다(→ CPP-RELOC-002, §3.1).
- 증거: `cpp/src/runtime/host/app.cpp:2852-2854, 2959-2960` (terminal 저장), `2741-2758, 3072-3077, 3291-3296` (단발 조회)
- 구현 checkpoint `9fc3179a68`: `relocated` 결과만 terminal로 보존하고, `blocked` 결과는 waiter 완료 뒤 operation을 다시 시작 가능한 상태로 되돌린다. 다음 호출은 이전 worker thread를 join한 뒤 preflight부터 다시 실행한다. `test_cpp_framework_target_contract`와 기본 `test_cpp_framework_host_lifecycle`는 통과했다. 실제 재시도 process 회귀를 추가해 실행하는 과정에서 기존 optional fixture가 readiness payload를 보내기 전에 `No serializer is registered for this payload type`으로 실패했으므로, serializer owner gap을 우회하지 않고 재시도 process 증거를 보류한다.
- 구현 checkpoint `258a9aefcc`: preflight와 workload unit별 target 선택이 첫 descriptor snapshot만으로 `target_unavailable`을 확정하지 않는다. 기존 Location polling interval로 Store descriptor와 Core peer admission을 shared deadline까지 다시 확인하고, deadline이 끝나면 `Blocked/TargetUnavailable`을 유지한다. Source가 첫 empty snapshot을 읽은 뒤 100 ms 후 replacement host를 시작하는 회귀에서 descriptor 게시와 peer admission 뒤 relocation이 완료됐으며, target이 계속 없을 때 deadline만큼 기다리는 회귀도 통과했다. 전체 `test_cpp_framework_host_lifecycle`, `test_cpp_framework_contract_headers`, `test_cpp_framework_execution`, `test_cpp_framework_app_host`, `test_cpp_framework_m6b_runtime`, `test_cpp_framework_m6c_runtime`, `test_cpp_framework_target_contract`가 통과했다. 설치 package를 사용하는 별도 process 검증이 남아 있어 아직 종결하지 않는다.
- 검증 checkpoint `b07b20d318`: public `app_t::relocate()`를 같은 source host에서 두 번 호출하는 owner 회귀를 기본 test에 추가했다. Replacement가 없는 첫 호출은 100 ms 뒤 `Blocked/TargetUnavailable`로 끝나고 source는 `Serving`을 유지한다. Replacement host가 게시된 뒤 두 번째 호출은 이전 결과를 재사용하지 않고 preflight를 다시 실행해 `Relocated/None`으로 완료됐다. `test_cpp_framework_host_lifecycle`과 `test_cpp_framework_target_contract`가 통과했다. 설치 package를 사용하는 별도 process 검증은 계속 남아 있다.
- process checkpoint `1f4395bb9d`, `dce17f2d44`: Framework package를 빈 prefix에 설치하고 source tree include를 사용하지 않는 out-of-tree consumer를 build했다. Redis Store를 공유하는 source와 replacement를 별도 OS process로 실행했다. Source의 첫 호출은 `outcome=1 reason=1`(`Blocked/TargetUnavailable`)을 반환한 뒤 같은 process에서 `Serving`을 유지했다. 두 번째 호출은 replacement를 시작하기 전에 먼저 실행했으며, Store descriptor와 peer admission이 전파된 뒤 `outcome=0 reason=0`(`Relocated/None`)으로 완료됐다. 강화한 runner가 연속 두 번 통과했으며 최신 log는 `framework/languages/cpp/e2e/RelocationRetry/logs/20260808-015358-1174433`이다. 설치 Framework archive SHA-256은 `a54926a4d8518d2646a0757530d803a66c68abafbf5a604cf2d2e850e1a27384`, header 수는 112개다.

### CPP-TOPO-001 — 수동 피어 admission fence 미설치
자동 연결 루프는 descriptor의 `lifecycle_generation`/`security_identity`를 `expect_peer`/`connect_peer`로 설치하지만, **수동 연결 목록에 있는 엔드포인트의 descriptor는 명시적으로 건너뛴다**. 수동 피어 등록 경로는 endpoint + 선택적 RID만 받으므로 fence가 설치될 길이 없다. 해당 엔드포인트의 스테일/대체 노드가 descriptor fence로 거부되지 않는다. 이는 JVM에서 이미 수정·종결된 `JVM-TOPO-001`과 동일 계열 결함이다.
- 증거: `cpp/src/runtime/locations/location_auto_connect_host_service.hpp:161-164, 182-184`, `cpp/src/runtime/mesh/mesh_node_runtime.cpp:432-440`, `cpp/src/runtime/channels/route_channel_registration.cpp:53-77`
- 소스 checkpoint `2c60654066`: manual endpoint도 Store descriptor의 `lifecycle_generation`과 `security_identity`를 `mesh_node_runtime_t::expect_peer()`에 전달하고, 제거 시 `forget_peer()`를 호출하도록 수정했다. Manual endpoint의 physical connect/disconnect 소유권은 기존 RouteMesh registration에 남겨 descriptor fence와 연결 수명을 분리했다. `test_cpp_framework_target_contract`의 `CPP-TOPO-001` source gate와 `test_cpp_framework_store_location_resolvers`가 통과했다.
- process 증거: `ChannelEgressRouting/run_e2e.sh CH-E2E-01` 최신 실행(`framework/languages/cpp/e2e/ChannelEgressRouting/logs/20260808-050918-1033016`)은 manual RouteMesh 양방향 request와 현재 package를 통과했지만, stale descriptor를 의도적으로 주입해 negative fence를 확인하지는 않는다. 따라서 source 수정만으로 종결하지 않고, 대체 descriptor가 같은 manual endpoint에 연결되지 않는 process 회귀를 별도로 확보할 때까지 **PARTIAL·검증 중**으로 둔다.

### 2.1 의사결정 반영 뒤 추가된 public contract gap

다음 여섯 항목은 internals 문서의 구현 구조 차이가 아니라 정식 C++ exact interface와 설치되는 public header가 직접 다른 경우다. 따라서 source에 비슷한 내부 기능이 있는지만으로 충족 판정을 내릴 수 없다.

| ID | 분류 | 계약과 현재 구현의 차이 |
|---|---|---|
| CPP-CONTRACT-DIAG-001 | CLOSED | 구현 checkpoint `23812b9030`에서 public `message_flow_log_mode_t`를 exact interface의 `off=0`, `errors=1`, `normal=2`, `detailed=3` 네 값으로 교체하고 runtime, sample과 E2E 설정을 같은 이름으로 갱신했다. Legacy 다섯 값의 재노출을 막는 target contract와 exact numeric static assertion을 추가했다. Public header 의존 object 132개를 다시 만든 뒤 관련 unit·runtime test가 통과했고, package checkpoint `113e6a46b0`의 0.10.1 clean consumer도 네 값과 숫자를 compile-time으로 확인했다. Process checkpoint `476a630d32`에서 실제 RouteMesh handler가 대기하는 동안 `normal→off`와 `off→normal`을 바꿨다. 첫 request는 terminal `replied`까지 기록됐고 두 번째 request는 중간 변경 뒤에도 flow record를 만들지 않았으며, Redis 기반 다섯 process `MON-C1`이 두 번 통과했다. |
| CPP-CONTRACT-DIAG-002 | CLOSED | 선행 checkpoint `ca9683fd26`에서 RuntimeMonitoring `MON-C1`이 application logging provider로 structured `zlink.message_flow` record를 받고, `svc-throw` provider가 예외를 던져도 원래 Mesh request와 후속 observation이 성공하는 process 회귀를 추가했다. 구현 checkpoint `de592aa66f`에서는 raw flow/error DTO와 observer callback을 installed public header에서 제거하고 runtime 전용 `dispatch_events.hpp`로 옮겼다. 구현 checkpoint `c187ba3c79`에서는 `trace_log_file`·`trace_label`과 runtime 전용 live-mode getter·builder를 public header에서 제거했다. 진단 출력은 application logging provider가 소유하며, 기존 sample과 E2E의 별도 evidence file도 같은 logging 설정으로 유지한다. Public header 의존 object 132개를 다시 만든 뒤 관련 unit·runtime·cross-process 검증과 Redis 기반 `MON-C1`이 통과했다. Package checkpoint `113e6a46b0`에서 Core·C++ binding 0.10.1로 Framework를 clean configure·build했으며, 설치 header에서 제거 대상 export가 없는지 확인한 뒤 out-of-tree consumer를 compile하고 실행했다. |
| CPP-CONTRACT-QUERY-001 | CLOSED | 구현 checkpoint `3bd461e22b`에서 exact interface의 object kind·state·entry·filter 타입과 Actor·Spot exact lookup, bounded object page method를 public header와 Store-backed runtime에 추가했다. Missing은 빈 `optional`, reserved authority는 `creating`, active authority는 owner lease에 따라 `ready` 또는 `unavailable`을 반환하며 Store 실패는 전체 operation의 `Unavailable`로 보존한다. Page size `1..1000`, opaque continuation과 encoded page 4 MiB 상한을 적용하고, raw Store page가 filter와 맞지 않아 비어 있어도 다음 cursor를 읽어 요청한 수만큼 채운다. Owner-layer 회귀 36건, contract headers, target contract, app host, execution, M6B와 M6C가 통과했다. Redis 기반 multi-process `MON-A6`은 실제로 생성한 Actor·Spot의 exact 상태와 stable type을 조회하고 `pageSize=1` Actor 목록의 continuation을 다음 page로 이어 갔다. Core·C++ binding 0.10.1을 사용하는 clean Framework package에서 `verify_packaged_contract.sh`와 전체 install consumer가 새 public signature를 compile·link·실행했으며, archive SHA-256은 `7257d1f3d59a6b99e9bf8e1d0c97286d53cbb6db6f0e8f3695d576496033a61f`다. |
| CPP-CONTRACT-STREAM-001 | CLOSED | 구현 checkpoint `eefcda189d`에서 `stream_send_call_t::timeout(...)`을 추가하고, Core STREAM writer가 기록한 socket admission timeout을 호출별 값으로 더 짧게 제한하도록 연결했다. `1..INT_MAX` 범위를 벗어난 값은 modifier에서 거부한다. Owner-layer 회귀는 20 ms 제한이 1초 socket 기본값을 줄이는지, 만료 뒤 재시도하지 않는지, send-ready 신호 뒤 거부된 시도만 한 번 재제출하고 성공 뒤 추가 신호로 replay하지 않는지 확인한다. `test_cpp_framework_contract_headers`, `test_cpp_framework_stream_framework`, `test_cpp_framework_target_contract`가 통과했고, package checkpoint `113e6a46b0`의 0.10.1 clean consumer도 timeout modifier를 compile했다. Process checkpoint `4a5290bf67`의 `CPP-CONTRACT-STREAM-001`은 isolated Core·C++ binding 0.10.1 package로 Server STREAM, connector peer와 4 KiB TCP gate를 실행했다. Gate를 닫은 뒤 32 KiB packet 57건이 수락되고 58번째 호출이 설정한 20 ms에 `DeadlineExceeded`로 끝났으며, 닫힌 동안 `bytesReadAfterClose=0`을 유지했다. Gate를 다시 연 뒤 public submit과 byte forwarding도 복구됐다. 실행 log는 `framework/languages/cpp/e2e/SubmitAdmission/logs/20260808-011634-79768`이고 Core runtime SHA-256은 `cdf3faca4020e061299b6d4a725c6313fc0a3f55c81344776f93630a537e7490`, binding package tree SHA-256은 `76e99938a973141ec9c2d3d5886ee876a607fe735c6588c59ef93b10342bcdfd`다. |
| CPP-CONTRACT-ROLE-001 | CLOSED | 구현 checkpoint `4b741bc692`에서 Client runtime record가 없으면 target 대기 전에 `NotConfigured`를 반환하고, Client role은 있지만 ready target이 없는 경우만 `NotFound`를 유지하도록 selector를 분리했다. Process 검증에서 selector보다 먼저 실행되는 공통 channel preflight가 두 상태를 다시 `Unavailable`로 합치는 결함을 확인했고, checkpoint `49027d944f`에서 Client role이 없는 send와 request는 connection 검사 전에 `NotConfigured`로 끝나도록 owner runtime을 수정했다. `test_cpp_framework_client_server_runtime`은 `assert`가 비활성화된 빌드에서도 server-only send와 request의 오류 kind를 검사하며, `test_cpp_framework_channel_messaging`과 `test_cpp_framework_target_contract`도 통과했다. Common CH-E2E-05 process는 server-only `errorKind=3`, 정상 Client request 성공, 마지막 ready target 제거 후 `errorKind=0`을 한 흐름에서 확인했다. 실행 log는 `framework/languages/cpp/e2e/ChannelEgressRouting/logs/20260808-013425-618773`이다. Clean install prefix의 out-of-tree consumer도 실행됐고 Framework archive SHA-256은 `a54926a4d8518d2646a0757530d803a66c68abafbf5a604cf2d2e850e1a27384`다. |
| CPP-CONTRACT-HTTP-001 | CLOSED | 구현 checkpoint `14c5f04f03`에서 `http_options_builder_t::snapshot()`과 `validate()`를 private으로 옮기고 host의 `app_t`와 `zlink_framework_options_t`만 접근하도록 제한했다. Application surface에서 두 method가 보이지 않는 negative compile assertion을 추가했고 `test_cpp_framework_contract_headers`, `test_cpp_framework_app_host`가 통과했다. Package checkpoint `113e6a46b0`에서는 0.10.1 install tree만 참조하는 consumer에 같은 negative compile assertion을 넣고 framework·HTTP client·stream connector와 함께 compile·link·실행했다. |

### 2.2 의사결정 검토에서 gap으로 추가하지 않은 항목

- STREAM 인증은 application callback의 책임이다. Framework public auth gate가 없다는 사실은 gap이 아니다(DEC-07).
- relocation의 target 선택, 일부 Actor만 옮기는 modifier와 public packet sequence/observer는 계약에서 제외됐다. 이 API들이 없다는 이유로 gap을 추가하지 않는다(DEC-10, DEC-12).
- logical disconnect는 기존 unbind 의미를 강화하는 대상이다. 별도 `Unbind` 이름을 추가하지 않는다(DEC-11).
- Message Follow 중복 억제의 내부 알고리즘은 언어별 재량이다. 안전 결과를 만족하는 한 특정 suppression helper가 없다는 이유로 gap으로 분류하지 않는다(DEC-13).
- C++ HTTP `snapshot()`/`validate()`의 public 노출은 gap이지만, 별도 public snapshot type을 새 계약으로 만드는 방식으로 해결하지 않는다(DEC-14).
- participant application state는 source에서 64 MiB 상한과 `StateIncompatible` 경로를 확인했다. 다만 경계값 process E2E를 실행하지 않았으므로 완료 증거로 승격하지 않는다(DEC-17).

### 2.3 승인된 계약 변경과 구현 대기 항목

Session 교체 정책은 정식 spec과 shared wire schema에 먼저 반영했다. 각 언어 구현과 cross-language 검증이
끝나기 전에는 gap을 종결하지 않는다. 나머지 항목은 새 계약 판단 없이 기존 schema conformance와 E2E fixture를
수정한다.

- `CPP-SESS-001`: 공용 schema 변경 없이 command 36/38 codec, correlation, active/tombstone transition과
  expected binding generation 검증을 구현했다. 남은 작업은 같은 node·cross-node·stale·duplicate·pre-restart
  matrix와 설치 package process 증거이며, spec 승인 대기 항목이 아니다.
- `CPP-SESS-004`: `boundSessionReplaced(51)` 송수신, 이전 session owner lifecycle과 binding exact identity 검증,
  callback 성공·실패 terminal 100 ms 뒤 Framework close를 구현했다. Outbound queue가 먼저 비어도 시간을 줄이지
  않으며 non-blocking timer로 예약하고 callback turn을 즉시 반환한다. sleep·blocking wait·session lane·worker
  점유, ACK 대기와 bind rollback은 사용하지 않는다. SM-D6는 통과했고 stale·duplicate·pre-restart와 다중
  Actor cleanup matrix가 남아 있다.
- `ST-F4/F5` Message Follow E2E: server fixture가 HTTP DTO의 `node_rid`와 generation을 버리고 ActorId를 다시
  조회하므로 old-ref 의미를 검사하지 않는다. F4의 caller terminal은 G2를 request로 바꿔 `Unavailable`을
  확인하고, F5도 exact old-ref를 실제 transport에 사용해야 한다. F1/F2는 이동 중 순서와 추월 방지 단계를
  별도로 audit한 뒤 상태를 결정하며 이 결함만으로 미구현으로 내리지 않는다. Public API는 변경하지 않는다.
- `CPP-WIRE-001`: decoder 엄격도는 공용 schema에 이미 확정되어 있으므로 별도 계약 결정을 기다리지 않는다.
  `golden/authority-key-v1.json`의 정상 vector를 C++ test에서 직접 읽고 leading zero·비정규 escape·잘못된
  UTF-8·1..255 길이 위반을 negative test로 추가한다. `CPP-WIRE-004`, `CPP-WIRE-006`의 cross-language fixture와
  설치 package 검증은 별도 후속 gate로 유지한다.

정식 spec·shared schema·generated asset의 변경이 필요하다는 판단이 새로 생기면 해당 구현을 임의로
진행하지 않고 이 보고서에 blocker로 등록한다. blocker가 등록된 뒤에도 계약 변경과 무관한 source test,
package/clean-consumer, 설치 package process, 다른 E2E gate는 계속 진행한다.

#### OPEN-CPP-SESSION-ROUTE-PROPAGATION — C++ 내부 Session owner route 전달 누락

다른 언어 구현과 정식 wire contract를 대조한 결과, 이 항목은 **spec 변경 blocker가
아니라 C++ 구현 gap**이다. Java의 직접 Actor Join 경로는 이미 존재하는
`sessionRelocationRoute(44)`에 Session owner node generation·owner ID·owner lease generation·
binding generation·relocation ID·high-water를 채워 target으로 전달한다
(`framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/
spots/ZLinkActorSpotAdmission.java:597-638`,
`framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/spots/ZLinkUserSpotRetireTargetEndpoint.java:822-856`). 공용 schema와 C++ codec에도 같은 필드가
이미 정의되어 있다.

반면 C++의 `spot_actor_commit_route_request_t`
(`framework/languages/cpp/framework/src/runtime/spots/spot_route_packets.hpp:64-98`)와
`actor_bound_session_route_t`가 직접 Join commit에서 Session owner fence와 relocation/high-water
정보를 보존하지 않고, `framework/languages/cpp/framework/src/runtime/spots/
spot_route_internal_dispatcher.cpp:415-438,538-556`와
`framework/languages/cpp/framework/src/runtime/actors/actor_gateway_runtime.cpp:1542-1560`에서
node RID와 Session RID만으로 route를 만든다. 이는 기존 command 42/44/45를 C++ 내부 DTO와 target
commit까지 연결하는 작업이다. 추정 identity를 넣거나 공용 spec/schema를 바꿀 근거는 없다.

따라서 ST-F3A와 CPP-ROUTE-001은 이 C++ 내부 전달 작업과 이후 E2E 증거가 남아 있어 열린
상태지만, spec 결정 대기로 막힌 항목은 아니다. 사용자가 보류한 process E2E는 실행하지 않고,
구현 시에는 이미 정의된 route command의 값을 끝까지 보존하는지 source·owner-layer 회귀부터
확인한다.

#### OPEN-CPP-SESSION-OWNER-FENCE — 이전 session owner fence 보존 정합성

`boundSessionReplaced(51)`의 네 owner 값은 exact하게 검증해야 하지만, **Node RID와 node
generation을 ordinary local binding의 Session owner ID·lease generation으로 사용하는 것 자체는
spec 위반이 아니다.** Node는 local binding을 `sessionOwnerId=nodeRid`,
`sessionOwnerLeaseGeneration=nodeGeneration`으로 저장하고
(`framework/languages/node/packages/framework/src/runtime/foundation/service-stateful-runtime.ts:1568-1575`),
같은 값을 replacement 수신 시 exact하게 비교한다
(`.../service-stateful-runtime.ts:3197-3204`). .NET도 같은 기본값을 사용하면서 명시적인 owner
값이 있으면 보존한다
(`framework/languages/dotnet/src/Zlink.Framework/Runtime/Actors/ZLinkSessionActorBindingTable.cs:249-266`).
따라서 C++ `app.cpp:2107-2110`의 ordinary local replacement 기본값만으로 별도 spec blocker를
세울 수 없다. Golden vector의 서로 다른 숫자는 codec field separation을 검사하는 fixture이지,
모든 local binding에서 값이 달라야 한다는 의미는 아니다.

다만 Java는 relocation/handoff에서 inbound authority의 owner ID·lease generation을 별도로
보존하고 replacement에 다시 사용한다
(`framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/
binding/ZLinkJavaRawMeshNode.java:5520-5536`,
`framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaRawSpotNode.java:1940-1966`). C++에도
`public_host_runtime.cpp:3792-3917`의 route-owner resolver가 있으므로, route에 명시적인 owner
token이 있는 relocation 경로에서는 그 값을 `command 42/44/51`까지 잃지 않고 전달해야 한다.
이 요구는 위 C++ 내부 route propagation 작업에 합치며, 공용 wire나 spec 변경을 기다리는
blocker로 유지하지 않는다. ordinary bind의 default와 relocation의 explicit owner fence를
구분해 구현·검증한다.

#### OPEN-GATE-CPP-COMMON-E2E-INVENTORY — 공통 E2E 기준과 C++ gate baseline 불일치

공통 E2E heading은 `SF-C5A`를 포함해 375개인데 C++ inventory gate
`framework/languages/cpp/e2e/verify_common_inventory.sh:141-142`는 374개를 기대한다. 이는
현재 C++ gate의 baseline drift이며, 다른 언어에 동일한 inventory gate가 없다는 점까지 확인했다.
공통 E2E 문서와 feature-map은 보호 경로이므로 임의로 고치거나 `blocked` 항목을
`implemented`로 바꾸지 않는다.

따라서 이 항목도 public spec/schema blocker가 아니다. 사용자가 지시한 대로 추가 E2E 실행은
보류하고, 375 대 374 불일치와 feature-map/source/status 292개 조건을 열린 E2E gate로 기록한다.
향후 공통 문서 기준을 반영할 때는 별도 승인된 gate/document 작업으로 처리한다.

---

## 3. 문서별 상세

### 3.1 01-layering (레이어 경계와 식별자)

| ID | 분류 | 요약 |
|---|---|---|
| CPP-RELOC-001 | CLOSED | §2 참조 — blocked 결과 뒤 같은 source process의 재시도가 owner 회귀와 설치 package 기반 별도 process에서 `Relocated/None`으로 완료됐다. |
| CPP-RELOC-002 | CLOSED | 구현 checkpoint `258a9aefcc`에서 preflight와 unit별 target 조회를 Location polling interval과 shared deadline으로 묶었다. 첫 조회 뒤 replacement가 게시되고 admitted되는 owner 회귀와 deadline 종료 회귀를 포함한 전체 host lifecycle, contract headers, execution, app host, M6B, M6C, target contract가 통과했다. Process checkpoint `dce17f2d44`에서는 설치 package source가 두 번째 relocation을 먼저 시작하고 replacement process를 나중에 시작했으며, descriptor와 peer admission 전파 뒤 shared deadline 안에 `Relocated/None`으로 완료됐다. |
| CPP-LAYER-001 | PARTIAL·하 | 식별자 타입화가 절반만: `node_rid_t`/`actor_id_t`만 전용 타입이고 `spot_id_t`는 `std::string` alias, mesh/채널 이름은 평문 문자열 — 스펙이 명명한 안티패턴 그대로. 증거: `cpp/include/zlink/framework/contracts/spots/spot_identity.hpp:24` |
| CPP-LAYER-002 | PARTIAL·검증 중 | 구현 checkpoint `c2bc713c99`에서 진행 중 runtime call 식별자를 `call_id_t`로 바꾸고 내부 파일도 `call_id.hpp`로 옮겼다. 공개 Actor Join `OperationId`를 운반하는 `wire_operation_id_t`는 별도 strong type으로 정의해 같은 128-bit 표현을 쓰더라도 call ID와 암시적으로 대입되지 않는다. Foundation registry, host completion, mesh·ClientServer transport와 creation call site는 call 용어로 통일했고 compile-time assertion과 target source gate로 두 타입의 재결합을 막았다. 전체 `test_cpp_framework_operation_registry`, `test_cpp_framework_service_wire_codec`, `test_cpp_framework_m6a_runtime`, `test_cpp_framework_m6b_runtime`, `test_cpp_framework_m6c_runtime`, `zlink_cpp_framework_mesh_node_vertical_test`, `test_cpp_framework_target_contract`가 통과했다. 설치 package와 cross-process Actor Join completion 검증이 남아 있어 아직 종결하지 않는다. |
| CPP-LAYER-003 | PARTIAL·중 | 구현 checkpoint `ad335148f0`에서 handler와 deferred Join completion을 이미 직렬화하는 `actor_execution_queues`를 단일 owner로 유지하고, handler 실행 전체를 다시 잠그던 `actor_mailboxes` map·per-Actor mutex·수명 정리 코드를 제거했다. 추가로 Actor queue admission에서 진단 문자열을 매 packet마다 `name + "-actor"`로 이어 붙이지 않고 기존 work name을 이동해 전달하도록 정리했다. Deferred barrier ordering 회귀를 포함한 전체 `test_cpp_framework_execution`, `test_cpp_framework_m6b_runtime` 2회, `test_cpp_framework_target_contract`가 통과했다. turn당 `shared_ptr` 다수와 중첩 `std::function` 캡처는 남으므로 이 항목은 아직 종결하지 않는다. |
| CPP-LAYER-004 | PARTIAL·중 | 스트림 공개 계약에 core 바인딩 타입 누출: `stream.hpp`의 `compress/decompress/write_packet/reply_packet` 시그니처가 `zlink::message_t`를 노출 — 바인딩 메시지 타입 변경이 곧 공개 API 변경이 됨. 증거: `cpp/include/zlink/framework/contracts/streams/stream.hpp:103-104, 215-216` |
| CPP-LAYER-005 | PARTIAL·검증 중 | 구현 checkpoint `9ffc2af6ab`에서 teardown 결과를 최종 확정한 직후 `completion_admission->stop()`을 먼저 실행하고, 그 다음 termination state와 waiter 결과를 공개하도록 순서를 바꿨다. 따라서 terminal 결과를 관측한 뒤 새 completion이 admit되는 창이 없다. 순서를 고정하는 `test_cpp_framework_target_contract`와 전체 `test_cpp_framework_app_host`가 통과했다. Package host와 shutdown 중 completion 경합 process 검증이 남아 있어 아직 종결하지 않는다. |

만족 항목(요약): 책임 그래프/wrapper 금지/shutdown 순서·우선순위/동종 중복 작업 병합/등록 시점 검증/재시작 안전 호출 식별자 등은 충실. 성능 측정 항목은 PerfTests 실행 필요.

### 3.2 02-serialization (Spot·Actor 실행 직렬화)

| ID | 분류 | 요약 |
|---|---|---|
| CPP-EXEC-001 | GAP·중 | §4 "SpotWide 메시지 1건 처리 시 락 2회 미만" 위반이 현재 구조에도 남아 있다. Actor 전달은 `actor_execution_queue_snapshot`을 atomic으로 읽어 기존 Actor의 node `recursive_mutex` 조회를 제거했고, queue 생성·삭제 때만 copy-on-write snapshot을 갱신한다. 그러나 SpotWide 경로에서는 여전히 `spot_context_state_t::try_post_serial_async()`의 `callback_mutex`와 Spot queue mutex를 거치며, admission 경로의 중첩 lock과 close와 enqueue를 함께 선형화하는 무경합 gate가 없다. `serial_execution_queue_t::complete_one()`의 인접한 queue lock 두 번은 이번 점검에서 하나로 합쳤지만, 전체 항목은 GAP으로 유지한다. 증거: `cpp/framework/src/runtime/spots/spot_runtime.cpp:2682-2820,1353-1380`, `spot_runtime.hpp:228-242`, `cpp/framework/src/runtime/execution/serial_execution_queue.cpp:796-887`. |
| CPP-EXEC-002 | PARTIAL·검증 중 | 구현 checkpoint `fdb5173042`에서 Actor self-request와 같은 Spot execution gate를 기다리는 Actor·Spot request의 오류를 모두 `InvalidOperation`으로 통일했다. 제출 횟수가 0인지와 정확한 오류 kind를 확인하는 `test_cpp_framework_m6b_runtime`이 통과했다. Source·owner-layer 회귀는 통과했으며 cross-language process E2E의 오류 kind assertion이 남아 있어 아직 종결하지 않는다. |
| CPP-EXEC-003 | PARTIAL·검증 중 | coroutine `co_await` 뒤에도 Actor 실행 context가 유지되도록 ambient continuation snapshot에 Actor key와 Spot ID를 함께 보존했다. 따라서 재개 후 같은 Actor request가 `InvalidOperation`으로 종료되고 submission을 만들지 않는다. `try_create_actor`의 source turn inline 실행도 source와 target Spot이 같은 경우에만 허용하고, cross-Spot은 target serial lane으로 보낸다. `test_cpp_framework_m6b_runtime`에 coroutine self-wait와 submission 0/1 회귀를 추가해 통과했다. 설치 package와 cross-process 오류 kind assertion이 남아 있어 아직 종결하지 않는다. 근거: `cpp/framework/src/runtime/diagnostics/flow_context.cpp:25-76`, `cpp/framework/src/runtime/spots/spot_runtime.cpp:3510-3520`, `cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_m6b_runtime.cpp:602-645` |
| CPP-EXEC-004 | PARTIAL·중 | Pitfall 2 큐 포화 계열의 결과 보존은 보강했다. raw routed send의 `backpressured`·`terminated`·기타 `submit_result_t`를 public host까지 전달하고, local Actor와 local publish dispatch도 application message budget에서 `backpressured`를 반환한다. Spot mesh send·publish와 Actor relay도 결과를 `internal_failure`로 축약하지 않고 기존 Framework error kind로 매핑한다. 따라서 `async_submit_runtime`의 send-timeout 재시도와 `DeadlineExceeded` 변환이 Actor/Spot one-way에도 적용된다. 다만 같은 런타임의 포화→deadline process assertion과 모든 one-way surface의 독립 회귀가 남아 있어 종결하지 않는다. 근거: `cpp/framework/src/runtime/backend/raw_route_port.cpp:77-119`, `cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp:934-969,1220-1309`, `cpp/framework/src/runtime/stateful/public_host_runtime.cpp:794-830,2370-2420,6127-6188`, `cpp/framework/src/runtime/spots/spot_runtime.cpp:2323-2435,7428-7435`, `cpp/framework/src/runtime/mesh/mesh_node_runtime.cpp:2405-2425`; 단위/ToActor/ChannelEgress 회귀는 §7.3에 기록한다. |

만족 항목(요약): per-actor 큐→공유 gate 구조, PerActor 모드, 타이머 lane, 2-lane FIFO 이중 한도, lifecycle 우선+burst 8+yield debt, 앞끼워넣기 금지, 실행 자원 비비례 등 핵심 직렬화 구조는 충실.

### 3.3 03-progress-isolation (앱/인프라 실행 분리)

| ID | 분류 | 요약 |
|---|---|---|
| CPP-DISP-002 | CLOSED | §2 참조 — owner-layer HOL 제거, public `CapacityExceeded`, 독립 owner와 health 진행, drain과 recovery를 owner 회귀와 isolated-package process E2E에서 확인함 |
| CPP-DISP-001 | CLOSED | §2 참조 — owner-layer 회귀와 isolated-package actual-pump saturation process E2E에서 `CapacityExceeded`, health 유지, drain과 recovery를 확인함 |
| CPP-DISP-003 | PARTIAL·검증 중 | 구현 checkpoint `807a87896d`에서 미승인 peer의 bounded application queue가 포화되면 Request header의 correlation과 transport request sequence를 읽어 기존 reply 경로로 `workerQueueFull` terminal을 즉시 반환하도록 수정했다. one-way 메시지는 bounded drop을 유지하며 trace에서 terminal reply 성공과 drop을 구분한다. 실제 transport request 1,025개를 admission 전에 전송해 앞의 1,024개는 보류되고 마지막 Request는 timeout 전에 terminal reply를 받는 회귀를 추가했다. 전체 `test_cpp_framework_m6b_runtime` 2회와 `test_cpp_framework_target_contract`가 통과했다. 설치 package와 실제 양방향 handshake 경합 process 검증이 남아 있어 아직 종결하지 않는다. |
| CPP-DISP-004 | PARTIAL·검증 중 | 구현 checkpoint `7f2bf358fe`에서 caller task가 worker dequeue 시점에 성공으로 끝나는 기존 경계는 유지하되, 이후 publisher 반환 실패와 모든 예외를 fallback log와 누적 counter로 남기도록 executor를 수정했다. 표준 Spot publish는 fallback과 중복 기록하지 않고 `route_mesh_channel/publish/drop` structured event에 packet·channel·topic과 예외를 기록한다. Generic `publish_call_t`의 완료 후 실패 counter 회귀, structured observer 필드 회귀, 전체 `test_cpp_framework_messaging`, `test_cpp_framework_execution`, `test_cpp_framework_target_contract`가 통과했다. 설치 package와 실제 RouteMesh publish 실패를 사용하는 process 검증이 남아 있어 아직 종결하지 않는다. |

만족 항목(요약): 실행 영역 2분할(펌프 스레드/앱 executor), 2도메인 mailbox, 3중 한도 공존, 관찰 비차단, send 계열 backpressure 절차, 수신 HWM의 앱 한정 정지 등은 충실.

### 3.4 04-completion (완료 확정)

| ID | 분류 | 요약 |
|---|---|---|
| CPP-COMP-001 | PARTIAL·검증 중 | 구현 checkpoint `1db9758177`에서 Actor 이동 중 `Unavailable`에 내부 `actor_transfer_in_progress` origin을 부여하고, error envelope metadata를 통해 typed origin을 보존하도록 바꿨다. 재시도는 `what()` 문구 대신 이 origin만 확인한다. Native 예외는 `std::system_error`의 transport `errc`만 `Unavailable`로 분류하고 다른 `std::exception`은 `InternalFailure`로 처리하므로 `not connected`, `stale`, `errno=113` 문자열 검색이 남지 않는다. 실제 `channel_reply_writer_t`를 거친 origin roundtrip과 관련 CTest가 통과했다. 설치 package와 Actor 이동 중 request 재시도 process E2E가 남아 있어 아직 종결하지 않는다. |
| CPP-COMP-002 | CLOSED | 재검토 결과 이 항목은 서로 다른 owner 경계를 하나의 primitive로 합치라는 과잉 판정이었다. `operation_registry_t`는 transport callback·deadline, `exactly_once_table_t`는 replay/dedup terminal, `task_shared_state_t`는 public task continuation, `pending_operation_state_t`·`submit_once_t`는 caller submit/cancellation 경계를 각각 소유한다. 각 경계는 claim-once를 보장하고 mesh request의 단계별 chaining은 중복 terminal ownership이 아니라 transport·public task·dedup 결과의 분리된 전달이다. 공통 spec은 terminal-once와 operation identity를 요구하지만 단일 내부 자료구조를 요구하지 않는다. `test_cpp_framework_operation_registry`, M6B/M6C와 target contract가 통과했다. |
| CPP-COMP-003 | PARTIAL·중 | public host가 completion operation을 먼저 만들고 `operation.low`를 raw Mesh request의 correlation 입력으로 전달하도록 연결했다(`public_host_runtime.cpp:2558-2642`, `raw_mesh_node_owner.cpp:901-949`). 따라서 transport 응답 등록은 submit보다 앞서고 correlation을 transport가 새로 만드는 경로가 줄었다. 다만 host의 `_completed_operations` holding table과 `wait_for_completion()` 조회·cv 대기는 남아 있고, 모든 infrastructure request가 같은 입력 경로를 사용하는지와 holding-slot 제거는 아직 완료하지 않았다. |
| CPP-COMP-004 | PARTIAL·검증 중 | timeout 뒤 늦게 도착한 terminal이 `_completed_operations`에 고아 entry로 다시 삽입되지 않도록 bounded tombstone을 유지하고, holding table가 가득 찬 경우에는 `_completion_overflow_operations`에 bounded identity를 보관해 waiter가 `CapacityExceeded`를 관찰하도록 바꿨다(`mesh_node_runtime.cpp:2619-2675,2827-2852`). 65,536개 상한과 stop cleanup은 유지한다. 전체 table saturation process와 overflow 스트레스 회귀가 남아 있어 종결하지 않는다. |

만족 항목(요약): 단일 finalize 경로, 락 밖 콜백, 등록 선행-제출 후행, 수락 후 자동 재전송 금지, fire-and-forget 완료 시점 등은 충실.

### 3.5 05-relocation-continuity (이동 중 메시지 연속성)

| ID | 분류 | 요약 |
|---|---|---|
| CPP-FOLLOW-001 | PARTIAL·검증 중 | 구현 checkpoint `4d5a797dc0`에서 Message Follow route 확인과 quota 예약을 한 lock 안의 typed result로 합쳤다. Route 부재는 정상 direct route 진행, 기간 만료와 hop 한도는 `Unavailable`, generation 불일치는 `InvalidOperation`, 1,024건·16 MiB 한도는 `CapacityExceeded`로 구분한다. Actor route fence도 local Actor의 object generation만 어긋난 경우 `InvalidOperation`을 반환한다. Relay마다 내부 metadata에 방문 node를 최대 8개 보존하고 현재 node나 다음 target이 이미 포함되어 있으면 hop 한도에 도달하기 전에 `Unavailable`로 끝낸다. 종류별 coordinator 회귀와 source contract를 포함한 전체 execution, cross-process mesh vertical, M6B, M6C, target contract가 통과했다. Cross-language process E2E에서 caller가 세 오류 kind와 3-node loop를 관찰하는 assertion이 남아 있어 아직 종결하지 않는다. |
| CPP-FOLLOW-002 | PARTIAL·중 | §1 스팬 ② "보류 후 새 소유자 인계": one-way는 완전 구현이나 **Request는 backlog에 추가하는 동시에 호출자에게 retriable `unavailable`로 실패를 반환** — 이동이 호출자에게 노출됨. 연속성이 프레임워크 내장 클라이언트의 재시도(그마저 CPP-COMP-001의 문자열 키)에 의존. 증거: `cpp/src/runtime/spots/spot_runtime.cpp:5363-5397` |
| CPP-FOLLOW-003 | PARTIAL·검증 중 | 구현 checkpoint `89e3199771`에서 target Spot serial lane이 remote Actor prepare의 기존 pending admission 조회, user admission callback과 coordinator 등록을 한 turn 안에서 처리하도록 바꿨다. 같은 transfer ID·Actor fence·source/target Spot·completion OperationId로 prepare가 다시 오면 callback을 재실행하지 않고 첫 admission reply를 그대로 반환한다. 같은 transfer ID에 다른 identity나 OperationId가 오면 `ProtocolError`를 유지한다. 동일 prepare 2회가 같은 reply를 받고 callback count가 1인지, 다른 correlation은 거부되는지 확인하는 회귀를 포함한 전체 `test_cpp_framework_execution` 2회, `test_cpp_framework_m6c_runtime`, `test_cpp_framework_target_contract`가 통과했다. `test_cpp_framework_m6b_runtime`은 반복 중 1회 통과했고 이 변경과 무관한 owner-admission deadline assertion이 1회 실패했다. 설치 package와 실제 prepare reply 소실 process 검증이 남아 있어 아직 종결하지 않는다. |

만족 항목(요약): prepare 순서, 1024건/16 MiB 한도, 재생 순서, per-Actor 분리, follow 상수·라우트 교체, 조건부 원자 소유자 전환, 전환 후 롤백 금지, actor 이동 단일 phase enum 등은 충실.

### 3.6 06-routing-and-cache (대상 선택과 라우트 캐시)

| ID | 분류 | 요약 |
|---|---|---|
| CPP-TOPO-001 | PARTIAL·검증 중 | 소스 checkpoint `2c60654066`에서 manual endpoint에 Store descriptor의 lifecycle generation과 security identity fence를 설치하고 제거 시 해제하도록 수정했다. Source gate와 resolver test는 통과했으며 설치 package와 실제 stale manual peer negative process E2E가 남아 있다. |
| CPP-ROUTE-001 | PARTIAL·상 | Wire fence가 제공하지 않는 `owner_id`는 빈 값일 때 비교하지 않고, 제공되는 node·object·authority owner generation과 owner lease generation은 계속 정확히 비교하도록 수정했다. Wire fence와 같은 입력으로 actor route cache가 무효화되고 다음 조회가 Store를 다시 읽는 owner-layer 회귀 test도 추가했다. 구현 checkpoint `5e31808ad3`; `test_cpp_framework_store_location_resolvers` 33/33 통과. ST-F4/F5 fixture는 exact old Actor ref와 request `Unavailable` terminal을 사용하도록 정정했고, ST-F4·F5와 current global route 수렴을 `framework/languages/cpp/e2e/SpotActorTransfer/logs/20260808-172739-4014158/`에서 모두 확인했다. ST-F5의 기존 실패는 production route 결함이 아니라 A→B→A 절차가 공통 F5 계약과 달랐던 fixture 오류였다. ST-F3A에는 기존 command 42/44 값을 C++ 내부 commit까지 보존하는 구현 gap과 후속 E2E가 남아 있어 **PARTIAL·검증 중**이다. 이는 spec blocker가 아니다. |
| CPP-ROUTE-002 | PARTIAL·검증 중 | 구현 checkpoint `343a9c831b`에서 direct-store fallback도 공통 `live_location_reader_t`로 authority의 owner lease와 fencing margin을 확인하고, cache 수명을 `route_cache_max_age`와 owner admission lifetime 중 짧은 값으로 제한했다. Store 시각을 읽기 전의 `steady_clock` 시각으로 절대 만료 시각을 고정하므로 변환 과정에서 lease deadline을 연장하지 않는다. 10초 cache를 설정한 두 host 회귀에서 owner admission deadline 전 첫 전송은 `ok`, deadline 뒤 두 번째 전송은 `not_found`인지 확인했고 전체 `test_cpp_framework_m6b_runtime`과 `test_cpp_framework_target_contract`가 통과했다. 설치 package와 실제 Location Store를 사용하는 process 검증이 남아 있어 아직 종결하지 않는다. |

만족 항목(요약): positive 캐시+전체 fence, 미존재/생성중/스토어 실패 비캐시, 수명 검증, follow 와이어 레코드, smooth WRR+tiebreak, 사전 계산 사이클, 직접 지정 대상 불변, publish 스냅샷 등은 충실.

### 3.7 07-dispatch-loop (수신·디스패치 루프)

| ID | 분류 | 요약 |
|---|---|---|
| CPP-TIMER-001 | CLOSED | 구현 checkpoint `80be9877f2`에서 전달 tick과 실패 기록을 각각 최근 256개로 제한했다. Owner 회귀는 300회 추가 dispatch 뒤 history 크기 256과 마지막 scheduled index 305를 확인한다. Process checkpoint `d16439cd27`의 `SM-E4`는 실제 Spot timer를 세 overrun policy로 실행했고 `scenario SM-E4 evidence passed`로 끝났다. 같은 library를 빈 prefix에 설치한 clean consumer도 실행됐으며 Framework archive SHA-256은 `b3a93af95b7eccd3677032a020111a22758dda7d2a2cc0ec0ab581e416eeefcf`, 설치 header 수는 112개다. |
| CPP-DISP-005 | PARTIAL·검증 중 | 구현 checkpoint `388d59516b`에서 MeshNode ROUTER poller에 기존 `runtime_wake_timer_t`를 연결하고, local publish·Actor join·Actor message·Spot request enqueue가 같은 activity signal을 사용하도록 통합했다. 5초 poll 대기가 local publish 직후 500 ms 검증 한도 안에 반환되는 owner-layer 회귀를 추가했다. 전체 `test_cpp_framework_m6b_runtime`과 `test_cpp_framework_target_contract`가 통과했다. 실제 host loop의 local request latency 계측과 package process 검증이 남아 있어 아직 종결하지 않는다. |
| CPP-DISP-006 | PARTIAL·검증 중 | 구현 checkpoint `e401806c6c`에서 세 per-Spot post 경로가 close/idle-eviction admission flag를 확인한 뒤 `callback_mutex`를 유지한 상태로 serial queue enqueue까지 끝내도록 바꿨다. 따라서 sealing은 확인과 enqueue 사이에 들어오지 못하며 handler 실행은 기존처럼 lock 밖에서 진행된다. 전체 `test_cpp_framework_execution`과 `test_cpp_framework_target_contract`가 통과했다. 설치 package와 close·relocation이 실제 ingress와 경합하는 process 검증이 남아 있어 아직 종결하지 않는다. |
| CPP-TIMER-002 | CLOSED | 구현 checkpoint `80be9877f2`에서 `catch_up_bounded`가 한 dispatch 안에서 최대 설정 개수까지 연속 tick을 전달하고, 오래된 누락분은 첫 tick의 `skipped_ticks`에 기록하도록 수정했다. `max_catch_up_ticks`는 `1..INT_MAX`만 허용한다. 5개 만료를 상한 3으로 dispatch해 index 3·4·5와 skip 2를 확인하는 owner 회귀는 통과했다. 첫 process 실행은 native timer의 각 만료가 serial queue에 따로 들어가 production 경로에서는 `fire_count=1`만 전달되는 결함을 확인했다. Checkpoint `d16439cd27`에서 callback 실행 전과 실행 중에 쌓인 만료 수를 하나의 후속 dispatch로 합치고 overflow는 `UINT64_MAX`에서 제한했다. 같은 checkpoint에서 자동 DI 등록이 같은 handler type을 여러 surface에 중복 등록하지 않도록 고쳤고, `SM-E4` fixture에 명시적 RouteMesh role과 fanout routing id를 설정했다. `test_cpp_framework_execution`, DI·contract header 회귀, 실제 `SM-E4` process와 clean package consumer가 통과했다. 최신 process log는 `framework/languages/cpp/e2e/SpotService/logs/20260808-020853-1542739`이다. |
| CPP-TIMER-003 | PARTIAL·중 | §7 타이머 자원 비비례 위반: 스케줄링은 공유 표준형이나 등록당 `zlink::timer_t`가 signaler(OS fdpair)를 즉시 생성 — 10,000 Spot × 2 타이머 = 약 40,000 fd, 등록 수에 선형인 OS 자원. 증거: `cpp/src/runtime/timers/timer_runtime.cpp:116-121`, `core/src/api/monitoring/timer_api_internal.hpp:26-47`, `core/src/runtime/core/signaler.cpp:90-93` |

만족 항목(요약): ready set 상태화, mailbox 단일 스팬(check+insert), claim serial 배타, 10 ms 시간예산 배칭, 배치 수신 3중 한도, 커넥션 rotation cursor, overrun 3정책 이름 일치 등은 충실.

### 3.8 08-object-lifecycle (객체 종류와 활성화)

| ID | 분류 | 요약 |
|---|---|---|
| CPP-LIFE-001 | PARTIAL·검증 중 | 구현 checkpoint `f27fadae2c`에서 일반 Actor·Spot message admission이 logical object ID, authority owner generation과 current owner lease를 확인하고 `ObjectGeneration`은 비교하지 않도록 수정했다. 승인된 frozen record에는 current incarnation generation을 기록한다. Message Follow와 bound-session control은 exact Actor generation을 계속 요구하며 bound-session 불일치는 `invalid_operation`으로 끝난다. Stale generation 일반 Actor request 전달·reply, current generation 정규화와 stale lease 거부 회귀를 포함한 전체 M6B, execution, cross-process mesh vertical, M6C와 target contract가 통과했다. 첫 M6B 실행은 기존 route-cache deadline timing assertion에서 한 번 실패했고 같은 binary 재실행은 통과했다. 설치 package에서 객체 재생성과 ingress가 겹치는 process E2E가 남아 있어 아직 종결하지 않는다. |
| CPP-LIFE-002 | PARTIAL·검증 중 | 구현 checkpoint `af11dabeac`에서 explicit close와 idle eviction을 같은 lifecycle operation으로 통합하고, `OnClosing` 완료 뒤 location과 local index를 해제하도록 순서를 고쳤다. Callback이 실패해도 release를 수행한 뒤 예외를 다시 전달한다. Idle-eviction 회귀가 callback 실행 중 location/context가 유지되고 완료 뒤 제거되는지 확인하며 전체 `test_cpp_framework_execution`이 통과했다. 실제 concurrent Instance activation process E2E가 남아 있어 아직 종결하지 않는다. |
| CPP-LIFE-003 | PARTIAL·상 | 공개 계약인 failure spec §4.4는 `Ready` authority의 owner lease가 만료된 상태를 Missing과 구분하고, 다른 node의 cold activation을 시작하지 않은 채 bounded `Unavailable`로 끝내도록 요구한다. 소스 checkpoint `2c60654066`의 lease fence와 `ReadyAuthorityWithExpiredOwnerIsUnavailable` owner-layer regression은 통과했다. InstanceSpot runner는 이제 미구현 scenario를 정상 request로 처리하지 않고 fail-closed하며, IS-E2E-05에서 두 owner process 중 Ready owner를 `SIGKILL`한 뒤 lease 만료 후 후속 request가 `Unavailable`로 끝나고 양쪽 handler 미진입·surviving owner 미생성을 확인했다. 실행 log는 `framework/languages/cpp/e2e/InstanceSpot/logs/20260808-172816-4022130/`이다. 나머지 33개 scenario는 feature map에서 `blocked`로 남아 aggregate 전체 완료를 주장하지 않는다. 따라서 owner-loss scenario 자체는 구현됐지만 InstanceSpot 전체 process matrix가 남아 **PARTIAL·검증 중**이다. 증거: `cpp/src/runtime/locations/store_location_resolvers.hpp:390-421`, `cpp/src/runtime/channels/channel_runtime.cpp:2151-2187`, `cpp/src/runtime/stateful/public_host_runtime.cpp:1794-1918`, `framework/languages/cpp/e2e/InstanceSpot/Client/main.cpp:151-222` |

만족 항목(요약): 폐쇄 kind 집합, Entry Spot 이동 경계 제외, 생성 경합 단일 factory, 생성중 비캐시, 실패 생성 정리, count+byte 이중 한도, relocation 보류 상수 등은 충실.

### 3.9 09-session-binding (세션과 Actor 바인딩)

| ID | 분류 | 요약 |
|---|---|---|
| CPP-SESS-001 | PARTIAL·검증 중 | 정식 command 36/38 codec·transaction과 expected binding generation 검증을 구현했다. Focused CTest·SM-D6·package consumer는 통과했으며 stale·duplicate·pre-restart와 full cross-node matrix가 남아 있다. SupportChat에서 Join 중 `moving` Actor에 도착한 initial bind가 실패하는 lifecycle 경계도 남아 있다. |
| CPP-SESS-004 | PARTIAL·검증 중 | 즉시 bind 뒤 이전 exact session의 command 51 one-way 통지, callback, callback terminal 100 ms 뒤 non-blocking Framework close를 구현했다. SM-D6는 통과했으며 callback 실패·deadline·retry·다중 Actor cleanup matrix가 남아 있다. ordinary local binding에서 Node RID·lifecycle generation을 Session owner ID·lease generation의 기본값으로 사용하는 것은 Node/.NET과 정합하다. 다만 relocation/handoff route에 명시적인 owner token이 있으면 그 값을 보존해야 하므로 `OPEN-CPP-SESSION-ROUTE-PROPAGATION`과 연계한 C++ conformance가 남아 있다. spec blocker는 아니다. |
| CPP-SESS-002 | PARTIAL·하 | §2 "종류별 새 직렬 실행 primitive를 만들지 말라": 주 엔진(`serial_execution_queue_t`)으로 통합은 잘 되어 있으나 `service_mailbox_t`가 admission 예산·per-owner FIFO·ready set을 독립 재구현한 제2 primitive로 공존 — 스펙이 명명한 "two-domain mailboxes" 실패 패턴 구조. 증거: `cpp/src/runtime/mesh/service_mailbox.hpp:18-115` |
| CPP-SESS-003 | PARTIAL·검증 중 | 구현 checkpoint `8a709309bd`에서 공통 `serial_execution_queue_t`에 Spot·session·Actor 전달의 닫힌 lane policy 합 타입을 주입하도록 바꿨다. Spot 정책만 실행 방식과 `active`·반납 대기·이동 봉인 상태를 가지며, session 정책은 연결 열림·닫힘만, Actor 전달 정책은 별도 lifecycle 상태를 갖지 않는다. 기존 raw `allow_yield` 인자와 필드를 제거했고 yield 허용 여부는 Spot-wide 정책에서만 계산한다. Entry Spot, Spot-wide, per-Actor Spot, STREAM session과 Actor 전달 queue 생성 지점은 각각 이름이 있는 정책을 선택한다. 잘못된 lifecycle 타입 조합을 만들 수 없는지 확인하는 회귀를 포함한 전체 `test_cpp_framework_execution`과 `test_cpp_framework_target_contract`가 통과했다. `test_cpp_framework_m6c_runtime`은 변경된 fixture를 포함해 compile은 통과했지만, 실행은 이 변경과 무관한 기존 `relocation providers must be configured once before host start`에서 중단됐다. 설치 package와 process lane 검증이 남아 있어 아직 종결하지 않는다. |

만족 항목(요약): 세션 gate/Actor gate 분리, 연결 identity 쌍, 스왑 시퀀스 필터, 재연결 fresh 구축, 이동 시 연결 유지·라우트만 갱신 등은 충실.

### 3.10 10-liveness-and-state (생존 판정과 상태 공표)

| ID | 분류 | 요약 |
|---|---|---|
| CPP-OBS-001 | PARTIAL·검증 중 | 구현 checkpoint `b3094a2a13`에서 Instance-Spot activation trace context를 diagnostics mode gate 뒤에서만 만들고, `message_flow_event_t`와 문자열은 tracer의 lazy builder 안에서 생성하도록 수정했다. Request의 async terminal event는 최초 mode가 `off`가 아닐 때만 필요한 context를 한 번 snapshot한다. `test_cpp_framework_app_host`, `test_cpp_framework_message_flow`, `test_cpp_framework_target_contract`이 통과했다. Allocation 계측과 process observability E2E가 남아 있어 아직 종결하지 않는다. |
| CPP-OBS-002 | CLOSED | 구현 checkpoint `68460a5d9f`에서 diagnostics level을 message entry의 ambient context에 snapshot하고, 모든 후속 tracer와 async continuation이 같은 값을 사용하도록 수정했다. `off`로 시작한 메시지는 flow ID를 할당하지 않지만 빈 flow 값과 level을 보존하므로 처리 중 level이 켜져도 일부 event만 기록하지 않는다. 반대 방향도 같은 규칙을 적용한다. Checkpoint `23812b9030`에서 level 이름을 exact interface와 맞췄으며 `normal→off`와 `off→normal` owner-layer 회귀를 포함한 관련 test가 통과했다. Package checkpoint `113e6a46b0` 뒤 process checkpoint `476a630d32`의 `MON-C1`은 실제로 block된 RouteMesh handler마다 correlation을 기록하고, handler 실행 중 level 변경이 message entry snapshot을 바꾸지 않는지 양방향으로 확인했다. |

만족 항목(요약): 5s/15s 단일 표준, 비즈니스 메시지의 기한 비연장, 체크 신호 앱 미도달, accept-and-fail-per-call과 7-state 폐쇄 집합을 확인했다. Diagnostics public surface와 message entry level snapshot도 package·process 검증을 마쳤다.

### 3.11 11-message-ownership (페이로드 소유권과 복사)

| ID | 분류 | 요약 |
|---|---|---|
| CPP-OWN-001 | PARTIAL·검증 중 | 구현 checkpoint `b1053aceda`에서 cached `serializer_t<T>`가 encode/decode 함수와 expected content type을 함께 소유하도록 바꾸고, classic Channel, RouteMesh, MeshNode와 Spot·Actor typed handler가 envelope content type을 비교한 뒤에만 deserialize하도록 통일했다. Mismatch는 `ProtocolError`로 끝나고 handler를 호출하지 않는다. Custom `application/avro`와 기본 `application/json`·`application/octet-stream` cache 회귀, mismatch owner 회귀와 target contract를 추가했다. Serializer registry, handler registry, contract headers, channel messaging, execution, app host, cross-process Mesh vertical, M6A, M6B, M6C와 target contract가 통과했다. 첫 aggregate M6B 실행은 87초 동안 종료되지 않아 중단했으며 같은 binary 단독 재실행은 통과했다. 설치 package와 서로 다른 codec의 process E2E가 남아 있어 아직 종결하지 않는다. |
| CPP-OWN-002 | PARTIAL·검증 중 | `raw_stateful_dispatch_t::ingest`는 이제 이미 검증한 typed `frozen_application_record_t`를 queue에 보관하고, `summarize_frozen_application_record()`로 delivery summary만 만든다. canonical frozen bytes는 relocation snapshot/replay 직렬화 경계에서만 `encode_frozen_application_record()`로 만든다(`cpp/framework/src/runtime/stateful/raw_stateful_dispatch.cpp:425-456`, `maintenance_runtime.cpp:659-669,1818-1828`, `service_wire_codec.cpp:3221-3338`). 정상 ingress의 canonical allocation과 claim 재디코드는 제거됐고 M6B/M6C·service-wire·target contract가 통과했다. 설치 package와 실제 process allocation/copy 계측은 E2E 보류로 남아 있어 종결하지 않는다. |
| CPP-OWN-003 | PARTIAL·검증 중 | 구현 checkpoint `4ba8a08bae`에서 `try_claim()`이 pending map의 decoded application payload와 stateful queue의 canonical turn을 delivery에 값 복사하던 경로를 각각 이동으로 바꿨다. 추가 checkpoint에서 ingress가 이미 검증한 frozen summary를 pending에 보관하고 claim 시 canonical queue bytes를 다시 decode하지 않도록 바꿨다(`raw_stateful_dispatch.hpp:96-103`, `raw_stateful_dispatch.cpp:424-441,523-535`). Claim 회귀와 target contract가 통과했다. Ingress에서 transport frame, decoded payload와 canonical frozen record를 동시에 보관하는 구조와 설치 package process 계측은 남아 있어 아직 종결하지 않는다. |
| CPP-OWN-004 | PARTIAL·검증 중 | 구현 checkpoint `f663c6250f`에서 기본 JSON serializer가 nlohmann JSON 결과를 `encoded_payload_t`에 직접 기록하고, 수신 시 그 byte span을 직접 parse하도록 바꿨다. 송신의 `message_t::from_json() → encoded_payload_t::from_raw()`과 수신의 `encoded_payload_t::to_raw() → parse_json()` 왕복을 제거했다. 정확한 JSON wire 값과 decode 회귀, source contract를 포함한 전체 serializer registry, messaging, channel messaging, execution, cross-process mesh vertical, M6B, M6C, target contract가 통과했다. 설치 package의 process allocation/copy 계측이 남아 있어 아직 종결하지 않는다. |
| CPP-OWN-005 | CLOSED | 구현 checkpoint `2fa0a29fc2`에서 application handler가 raw payload를 받던 `send_raw`, `raw_handler_t`, `payload_view_t`와 전용 `handler_kind_t::raw`를 제거했다. Public header에서 세 식별자의 재노출을 막는 target contract를 추가했고 handler registry, serializer registry, contract headers, layout, messaging, execution, app host, channel messaging, cross-process Mesh vertical, M6B, M6C와 target contract가 통과했다. Package checkpoint `113e6a46b0`에서 0.10.1 install header에 세 raw 식별자가 없는지 확인하고 typed public surface를 사용하는 out-of-tree consumer를 compile·실행했다. |
| CPP-OWN-006 | PARTIAL·검증 중 | 구현 checkpoint `2cb188e1c5`에서 이미 encoded payload를 소유한 `message_t`가 decode와 raw 변환 때 `encoded_payload_t`를 값으로 반환하던 경로를 제거했다. 내부 visitor는 payload를 `const` reference로 읽고 결과 reference를 반환하지 못하도록 compile-time으로 제한한다. Message Follow 크기 계산, fanout beacon 비교와 fanout application decode는 binding의 `bytes()` view를 사용한다. Actor·User Spot 생성 record처럼 새 소유 buffer가 필요한 경계도 `to_bytes()` 임시 vector를 거치지 않고 view에서 목적 buffer로 한 번만 복사한다. 이를 고정하는 target contract와 전체 serializer registry, messaging, cross-process mesh vertical, M6B, M6C runtime 검증이 통과했다. 설치 package를 사용하는 process copy 계측이 남아 있어 아직 종결하지 않는다. |
| CPP-OWN-007 | PARTIAL·검증 중 | `raw_stateful_dispatch_t::ingest`가 이제 encoded application envelope에서 HWM charge만 검증·계산한 뒤 `application_admission()`을 먼저 호출한다. 포화된 owner는 payload vector 생성, typed frozen body 구성과 canonical encode 전에 거부된다(`service_wire_codec.cpp:4028-4107`, `raw_stateful_dispatch.cpp:231-417`). Header·route fence 해석과 authority 조회는 admission 전에 남아 있어 source 비용이 완전히 0이 되지는 않는다. `verify_packaged_contract.sh`와 clean consumer는 최신 build에서 통과했지만, process allocation 계측은 아직 검증하지 않았다. |
| CPP-OWN-008 | PARTIAL·검증 중 | 구현 checkpoint `79c7f894d7`에서 custom serializer를 typed cache에 넣을 때 erased encode/decode 함수를 한 번 복사해 serializer state가 소유하도록 바꿨다. Checkpoint `b1053aceda`에서는 content type도 같은 cache가 소유하도록 확장해 typed handler decode가 registry map을 다시 조회하지 않게 했다. Registry object를 move한 뒤에도 기존 serializer가 동작하는 owner 회귀와 전체 serializer registry, messaging, channel messaging, execution, cross-process Mesh vertical, M6B, M6C, target contract가 통과했다. Dynamic outbound `encode_parts`와 `content_type(type)` 선택은 아직 메시지마다 type map을 조회하므로 이 항목은 종결하지 않는다. |
| CPP-OWN-009 | PARTIAL·검증 중 | 구현 checkpoint `985c7dadb4`에서 Spot 내부 route packet의 모든 byte field를 padding을 포함한 RFC 4648 Base64 문자열로 직렬화하도록 통일했다. 숫자 배열 팽창은 제거했고 strict decoder가 잘못된 길이·문자·padding·non-canonical pad bit를 거부한다. Known vector round-trip과 invalid input 회귀를 포함한 `test_cpp_framework_messaging`은 통과했다. Cross-language wire fixture와 실제 Spot→Actor process 검증이 남아 있어 아직 종결하지 않는다. |

만족 항목(요약): 큐 보관 중 프레임워크 소유, 핸들러 완료 후 해제, header 우선 판독·admission 전 미역직렬화(타입 역직렬화 기준), 송신측 선택 캐시(COW·lock-free) 등은 충실.

### 3.12 12-service-wire-protocol (서비스 와이어 프로토콜)

| ID | 분류 | 요약 |
|---|---|---|
| CPP-WIRE-001 | PARTIAL·검증 중 | 구현 checkpoint `abaad2368a`에서 RFC 3986 unreserved byte만 보존하고 나머지를 uppercase percent encoding하는 `zla1:<a|s>:<byte-length>:<encoded-id>` codec을 internal owner로 추가했다. Encoder는 네 언어와 유효 입력의 byte 결과가 같고, C++ unit test가 `framework/runtime/protocol/golden/authority-key-v1.json`을 직접 읽는다. Decoder는 leading zero·비정규 escape·잘못된 UTF-8·raw byte 길이 0/256을 거부하도록 schema 규칙에 맞췄으며 authority-key focused test, target contract와 전체 focused CTest가 통과했다. 설치 package와 다른 언어 runtime이 같은 provider를 사용하는 process 검증이 남아 있어 아직 종결하지 않는다. |
| CPP-WIRE-002 | PARTIAL·검증 중 | 구현 checkpoint `a2ef9db6f4`에서 `mesh_node_socket_config_t::max_message_size`, Application HWM의 MeshNode 전용 startup validation, RouteMesh topology descriptor의 `effective_max_message_bytes`와 admission wire field를 제거했다. ClientServer와 STREAM이 소유하는 독립적인 message-size 계약은 유지했다. Public header와 private topology에서 재노출을 막는 target contract, RouteMesh admission의 정확한 wire 길이 회귀를 추가했다. 변경된 public struct의 ABI를 일관되게 적용하도록 전체 `zlink_framework` static library를 다시 만든 뒤 contract headers, layout, service wire codec, app host, execution, cross-process Mesh vertical, M6A, M6B, M6C와 target contract가 통과했다. Package checkpoint `113e6a46b0`에서 0.10.1 Framework와 clean consumer도 통과했다. 다른 언어 RouteMesh peer를 연결하는 process 검증이 남아 있어 아직 종결하지 않는다. |
| CPP-WIRE-003 | PARTIAL·검증 중 | 구현 checkpoint `6e550851c4`에서 `message_t::parse_json()`과 기본 typed serializer가 공유하는 `framework-json-v1` parser/dumper 경계를 만들었다. UTF-8 BOM, root·nested duplicate property와 non-finite floating-point encode를 `ProtocolError`로 거부하며 이를 고정하는 target contract를 추가했다. Serializer registry, handler registry, contract headers, messaging, channel messaging, app host와 target contract가 통과했다. 임의의 DTO를 `nlohmann::json`으로 변환한 뒤에는 integer의 원래 C++ type width가 남지 않으므로 64-bit integer의 decimal string·범위 규칙은 아직 완전히 강제하지 못한다. 다섯 언어가 함께 사용하는 golden fixture도 남아 있어 아직 종결하지 않는다. |
| CPP-WIRE-004 | PARTIAL·검증 중 | 구현 checkpoint `985c7dadb4`에서 multicast frame, relocation state와 backlog, join snapshot, Actor packet과 bound-session payload를 모두 padding을 포함한 RFC 4648 Base64 문자열로 encode/decode하도록 수정했다. `test_cpp_framework_messaging`이 known vector `AAEC/f7/`의 정확한 wire 값과 round-trip, invalid input 거부를 확인했다. Source·owner-layer 회귀는 통과했으며 공통 cross-language wire fixture와 C++ package를 사용하는 process E2E가 남아 있어 아직 종결하지 않는다. |
| CPP-WIRE-005 | PARTIAL·검증 중 | 구현 checkpoint `fa0179f139`에서 단일 Actor와 aggregate relocation이 공유하는 generator가 Linux `getrandom`, Windows system-preferred BCrypt, Apple·BSD `arc4random_buf`로 non-zero 128-bit `RelocationId`를 만들도록 수정했다. OS CSPRNG가 실패하면 약한 난수로 대체하지 않고 relocation을 시작하지 않는다. 현재 프로세스가 발급한 ID는 relocation root 보존 기간과 같은 24시간 동안 유지하며 zero나 중복 candidate는 최대 64회 다시 만든다. Collision 재생성 owner 회귀와 source gate를 포함한 전체 M6B, M6C, execution, cross-process mesh vertical과 target contract가 통과했다. 첫 M6B 실행은 기존 owner-admission deadline timing assertion에서 한 번 실패했고 같은 binary 재실행은 통과했다. 프로세스 재시작 전에 Store에 남은 retained root ID를 열거하는 SPI가 없어 재시작 뒤 충돌을 직접 조회하지 못하므로 아직 종결하지 않는다. |
| CPP-WIRE-006 | PARTIAL·검증 중 | 구현 checkpoint `63d014d55c`에서 common schema generator가 C++ `request_terminal_result`와 framework error별 허용 terminal mapping을 생성하도록 확장했다. Codec의 101–113 mapping을 제거하고 generated `valid_terminal_failure`를 사용하며, `text16`·metadata와 endpoint·relocation reference·StoreVersion은 각각 schema가 생성한 `blobBytes`, `metadataBytes`와 전용 bound를 사용한다. Generator freshness와 schema validator, 전체 `test_cpp_framework_service_wire_codec`, `test_cpp_framework_target_contract`가 통과했다. 다른 언어 generated decoder와 C++ 설치 package를 함께 검증하는 aggregate gate가 남아 있어 아직 종결하지 않는다. |
| CPP-WIRE-007 | PARTIAL·검증 중 | 구현 checkpoint `b464a808af`에서 queue가 보관하는 canonical relocation envelope와 application HWM 예약값을 분리했다. 일반 application payload는 payload 길이를 사용하고, `ZLinkFrameworkMultipart`는 count·length와 header part를 검증만 한 뒤 마지막 application body part 길이만 계산한다. 계산한 immutable byte 값은 ingress record에 보존되며 relocation stage와 maintenance envelope decode에서도 복원되므로 source와 target의 admission 회계가 같다. 잘린 multipart 거부, 128-byte canonical record가 4-byte application payload로 예약되는 경계와 completion 반납 회귀를 추가했다. 전체 `test_cpp_framework_service_wire_codec`, `test_cpp_framework_m6b_runtime` 반복 실행, `test_cpp_framework_m6c_runtime` 반복 실행과 `test_cpp_framework_target_contract`가 통과했다. 설치 package와 실제 multipart process HWM 검증이 남아 있어 아직 종결하지 않는다. |

만족 항목(요약): StreamNode는 `max_message_size` 기본 64 KiB를 제공하고 Core STREAM socket과
frame assembler에서 6-byte prefix를 제외한 inbound client→server header+payload에 적용한다
(`cpp/include/zlink/framework/contracts/streams/stream.hpp:278-281`,
`cpp/include/zlink/framework/contracts/configuration/framework_options.hpp:1233`,
`cpp/src/runtime/streams/stream_host_service.cpp:775-822,1507-1508`). Core `EMSGSIZE`도 server
trace에 기록하고 peer를 종료한다
(`cpp/src/runtime/streams/stream_host_service.cpp:1573-1580,1980-1995,2840-2845,2888-2898`). 이 제한을
RouteMesh나 server→client에 확장하지 않는다. 그 밖에 스키마 생성 상수 소비, frame-0 prefix,
디코더 검증·절단 거부, 41 커맨드 공간 일치, messageFollow/DescriptorRevision/ClientServer
방향성/liveness/§8·§9 게이트 등 코덱 수준은 매우 충실하다. Wire gap은 키 포맷,
RouteMesh에 남은 금지 상한 surface·field, JSON 프로파일에 집중한다.

---

## 4. 코드만으로 확인 불가 — 후속 검증 필요 항목

| 항목 | 확인 방법 |
|---|---|
| 처리량/할당/락 경합 기준선 (01 §1, 02 §4, 11 전반) | `tests/Zlink.Framework.PerfTests` 실행 + 임계값 결정 |
| 서로 다른 Actor 핸들러 비동시 실행, PerActor 타이머 동시성 (02) | ContractTests의 TSan 실행 |
| 핸들러 정지 중 타임아웃/shutdown/신규 피어 진행 (03) | stuck-handler 런타임 테스트 |
| 이미 admit된 async I/O가 CPU worker 포화 중에도 진행하는지 (DEC-03) | CPU-bound handler로 worker를 포화한 process에서 STREAM·Channel I/O completion과 timer 진행 확인. 별도 public I/O worker option은 추가하지 않음 |
| back-to-back 도착 시 wake 횟수 < 메시지 수 (07) | 카운터 계측 부하 테스트 |
| 응답/타임아웃/취소/shutdown 동시 발생 시 완료 1회 (04) | 동시성 스트레스 테스트 |
| backlog 재생 + 클라이언트 재시도의 end-to-end exactly-once (05) | 대상측 dedup 회귀 테스트 |
| 스토어 §7–§11 순서 불변식 (12) | `maintenance_runtime`/`public_host_runtime` 전용 후속 감사 + M6 회귀 스위트 |
| liveness 토큰 CSPRNG 여부 (`std::random_device` 플랫폼 보증) (12 §5) | 플랫폼 보증 결정 필요 |
| 느린 관찰자에도 처리 속도 유지 (10) | 관찰자 지연 주입 벤치마크 |
| participant state 64 MiB 경계 (DEC-17) | 64 MiB 성공과 64 MiB 초과 `StateIncompatible` process E2E |
| Ready Instance owner loss (08, CPP-LIFE-003) | IS-E2E-05에서 owner process 종료·lease 만료·bounded `Unavailable`·handler 미진입을 확인했다. 나머지 InstanceSpot scenario는 fail-closed `blocked`이며 aggregate 전체 gate는 남아 있다 |
| 스키마 validator가 C++ 빌드를 실제 gate하는지 (12 §1) | 빌드 시스템 실행 확인 |
| public exact interface와 실제 설치 package 일치 | source consumer compile, install 후 clean consumer compile, `verify_packaged_contract.sh` 실행 |
| fanout 구독자가 앱 페이로드 수신으로도 기한을 연장하는 것의 적합성 (10) | Transport Liveness 스펙 오너 판정 |

---

## 5. 권고

1. **이 보고서에서 unique ID 관리** — 정식 spec에 구현 진행 기록을 다시 넣지 않고, 이 plan 문서가 남은 2 GAP과 45 PARTIAL 및 종결 14건의 증거를 소유한다. 특히 CPP-TOPO-001은 종결된 `JVM-TOPO-001`, CPP-COMP-001은 종결된 `NODE-ROUTE-001`과 비교하되 다른 언어의 종결을 C++ 완료 증거로 사용하지 않는다.
2. **수정 순서 제안**:
   - 1차 (거짓 성공 차단) 완료: InstanceSpot 미구현 scenario fallback을 차단하고 feature map을 만든 뒤 IS-E2E-05를 두 owner process로 구현했다. 나머지는 `blocked`로 유지한다.
   - 2차 (session conformance) 진행: CPP-SESS-001/004의 command 36/38·51, callback·non-blocking close 구현과 SM-D6를 확인했다. 전체 lifecycle matrix와 ST-F3A의 C++ 내부 route propagation 작업은 별도 gate로 진행하며, 공용 spec 결정을 기다리지 않는다.
   - 3차 (fixture·wire) 완료 checkpoint: ST-F4/F5 old-ref request terminal, authority golden vector와 schema negative case를 C++ test에 연결했다.
   - 4차 (남은 process gate): CPP-TOPO-001, CPP-ROUTE-001, CPP-WIRE-002/003, CPP-OWN-001, CPP-COMP-001의 설치 package와 cross-language/process E2E를 실행한다. ST-F3A는 C++ 내부 route propagation 구현과 사용자가 보류한 후속 E2E가 남아 있어 대기한다. 이는 spec blocker 대기가 아니다.
   - 5차 (자원·성능): CPP-COMP-004(고아 엔트리 누수), CPP-TIMER-003(fdpair), CPP-EXEC-001/CPP-OWN-002~004(복사·락 절감), CPP-DISP-005(100 ms wake)
   - 6차 (구조·명명): 나머지 PARTIAL
3. **오류 종류 정합은 계약 테스트로 고정** — CPP-EXEC-002와 CPP-FOLLOW-001은 공개 오류 kind가 계약과 달라지는 계열이므로, 수정과 함께 source contract test와 cross-language process E2E에 오류 kind assertion을 추가한다. CPP-CONTRACT-ROLE-001은 같은 방식으로 종결했다. CPP-LIFE-001은 오류 이름 문제가 아니라 일반 message admission fence 자체의 의미 차이다.
4. **public surface 제거에는 package 증거가 필요하다** — source header 수정만으로 끝내지 않고 install tree에서 제거된 export를 확인하고 clean consumer를 compile한다. Diagnostics observer 제거는 provider 경로와 failure-isolation E2E가 먼저 통과해야 한다.
5. 본 리포트는 정적 코드 읽기 기반이다. §4의 항목은 판정을 유보했다. GAP 항목은 source·exact interface 차이를 확인했지만 runtime semantics를 수정할 때에는 해당 경로의 contract test와 process E2E로 재현과 종결을 각각 증명해야 한다.

## 6. 이전 재검토의 검증 결과

현재 source로 관련 C++ target을 다시 빌드한 뒤 다음 8개 CTest를 실행했고 모두 통과했다.

- `test_cpp_framework_contract_headers`
- `test_cpp_framework_client_server_runtime`
- `test_cpp_framework_channel_messaging`
- `test_cpp_framework_stream_framework`
- `test_cpp_framework_message_flow`
- `test_cpp_framework_monitoring`
- `test_cpp_framework_location_runtime`
- `test_cpp_framework_http_integration`

이 결과는 기존 test가 현재 구현을 회귀시키지 않았다는 증거다. 이후 checkpoint의 항목별 source test 결과는 각 행에 기록했다. Package checkpoint `113e6a46b0`에서는 Core와 C++ binding 0.10.1을 사용해 Framework를 clean configure·build하고, `verify_packaged_contract.sh`와 전체 install consumer를 실행했다. Object query checkpoint `3bd461e22b` 뒤 같은 clean package를 다시 만들었으며 Framework archive SHA-256은 `7257d1f3d59a6b99e9bf8e1d0c97286d53cbb6db6f0e8f3695d576496033a61f`, 설치 header 수는 112개였다. Cross-language process E2E가 필요한 항목은 각 행에 계속 남겨 둔다.

## 7. 2026-08-08 Codex Sol 최종 전 점검 기록

### 7.1 검토 범위와 기준

- 검토 대상: C++ Framework gap·partial·public contract 상세 61행, C++ exact interface, 정식
  common spec/schema, production runtime, owner-layer regression, 설치 package/clean-consumer와
  실제 process E2E 증거.
- 모델/effort: 최종 종료 규칙에 지정된 동일 Codex Sol 모델·effort.
- 기준 commit: `137f2858bf7fd29f58405893473be8e773725a93`.
- 현재 후보는 commit하지 않은 `main` worktree다. 다른 작업자의 dirty 변경은 되돌리거나 stage하지 않았다.
- 공통 spec, shared schema, generated asset은 이 점검에서 수정하지 않았다.

### 7.2 이번 점검에서 확인·수정한 finding

1. STREAM에서 Actor relay envelope가 upstream correlation을 재사용해 replacement retry가
   exactly-once dedup 결과를 잘못 재사용할 수 있었다. `mesh_node_runtime.cpp`는 downstream
   `create_envelope`가 새 correlation을 만들도록 두고 upstream correlation 대입을 제거했다.
   `test_cpp_framework_target_contract`에 source regression gate를 추가했고 SM-D6 process에서
   새 session push와 old session replacement callback을 확인했다.
2. session replacement는 ACK·callback·close를 기다리지 않고 bind terminal을 반환하며, old
   session ingress는 즉시 거부하고 callback outbound write는 허용한다. callback terminal 뒤
   non-blocking timer로 100 ms를 보장하고, sleep·blocking wait·condition-variable wait·session
   lane/worker 점유를 사용하지 않는 경로를 확인했다. 이 금지를 `test_cpp_framework_target_contract`
   source gate로 고정했다. callback deadline, stale/duplicate/
   pre-restart, 다중 Actor cleanup matrix는 아직 실행되지 않았다.
3. ST-F5의 이전 실패는 production route가 아니라 공통 계약과 다른 A→B→A fixture였다. fixture를
   단일 A→B relocation과 exact old-ref request `Unavailable`로 고쳤고 ST-F1~F5 combined process
   log가 통과했다.
4. InstanceSpot 미구현 scenario가 정상 request로 통과하던 false green을 fail-closed로 바꾸고,
   IS-E2E-05를 두 owner process·`SIGKILL`·lease expiry·`Unavailable`·handler 미진입 증거로
   구현했다. 나머지 33개는 폐기하지 않고 `blocked`로 표시했다. `blocked`는 절차와 visible
   assertion을 추가해야 상태를 바꿀 수 있다는 뜻이다.
5. authority-key C++ decoder는 schema가 정한 leading zero·비정규 escape·잘못된 UTF-8·raw byte
   길이 1..255를 적용하고 공용 golden vector를 직접 읽도록 보강했다. focused test는 통과했지만
   다른 언어 provider를 함께 쓰는 process gate는 남아 있다.
6. 다른 언어 구현을 대조한 결과 직접 Actor Join의 정식 command 42/44에는 필요한 Session
   owner generation·owner ID·lease generation·binding generation·relocation ID·high-water를
   이미 전달하는 경로가 있다. C++는 `spot_actor_commit_route_request_t`와 target bind에서 이
   값을 버리고 있으므로 `OPEN-CPP-SESSION-ROUTE-PROPAGATION`이라는 C++ 내부 구현 gap으로
   재분류했다. 공용 spec/schema 변경 blocker로 보지 않으며, 추정값으로 ST-F3A를 통과시키지
   않는다.
7. `boundSessionReplaced(51)`의 ordinary local binding 기본값을 Node/.NET과 비교했다. 두
   언어 모두 Session owner ID를 node RID, lease generation을 node generation으로 기본 설정하고
   exact하게 검증한다. 따라서 C++ `app.cpp:2107-2110`의 기본값 자체는 spec blocker가 아니다.
   Java처럼 relocation/handoff에 명시적인 authority owner token이 있는 경우에는 그 값을
   보존해야 하므로, 이 요구를 `OPEN-CPP-SESSION-ROUTE-PROPAGATION`의 내부 conformance 범위로
   합쳤다. golden vector의 서로 다른 값은 codec 분리 검증으로 해석한다.
8. SupportChat에서는 conversation Actor가 `OnJoinedActor` callback과 같은 이동 경계에서
   `boundSessionBind(38)`을 받는다. `stateful_object_runtime_t::find()`가 `moving` 상태를
   Ready가 아닌 것으로 거부하는 현재 동작은 기존 `Remote Actor session binding did not
   complete successfully` 실패를 재현한다. moving Actor를 무조건 bind 허용하면 session
   process가 종료되는 회귀가 발생해 해당 실험은 원복했다. 현재는 spec/schema 변경 없이
   이동 중 bind의 owner fence·route 전환 시점을 정하는 런타임 finding으로 남겼다.
9. 공통 E2E inventory는 현재 문서의 `SF-C5A`를 포함해 375개를 집계하지만 C++ gate는
   374개를 기대한다. 다른 언어에는 동일한 inventory gate가 없으므로 이를
   `OPEN-GATE-CPP-COMMON-E2E-INVENTORY`라는 C++ gate baseline drift로 재분류했다. 이는 public
   spec/schema blocker가 아니다. 공통 문서와 gate를 임의로 맞추지 않고, 292개 feature-map/
   source/status 조건과 함께 사용자가 보류한 후속 E2E gate로 유지한다.
10. Actor 삭제 뒤 같은 global ID를 재생성할 때 provider가 authority key의 첫 번째 `:` 뒤 문자열을
    ID로 잘못 사용해 committed creation reservation을 찾지 못하는 결함을 확인했다. provider는
    이제 기존 strict authority-key decoder로 object ID를 복원한 뒤 authority 삭제와 reservation 삭제를
    같은 conditional write에 포함한다(`provider_location_repository.hpp:281-321`).
    `test_cpp_framework_opaque_store_providers`에 삭제→동일 ID 재예약·commit 회귀를 추가했고 단독
    CTest가 통과했다. ToActorMessaging 전체 TA-A1~TA-B3도 최신 binary에서 통과했지만 이 결과만으로
    `CPP-LIFE-002`의 concurrent Instance activation gate나 전체 aggregate를 종결하지 않는다.
11. `CPP-COMP-004`의 timeout 후 늦은 completion이 holding table에 다시 삽입되어 노드 정지까지
    남던 경로를 수정했다. `wait_for_completion`은 timeout operation을 bounded tombstone에 기록하고,
    `dispatch_ready`는 해당 identity의 늦은 completion을 버리며 tombstone을 제거한다. Tombstone과
    `_completed_operations` 모두 65,536개 상한을 가지며 stop 시 함께 비운다. `zlink_framework`,
    `test_cpp_framework_operation_registry`, `test_cpp_framework_m6c_runtime`, target contract와
    ToActor process 회귀가 통과했다. 이 변경은 correlation 선등록(CPP-COMP-003)과 table capacity
    초과의 caller-visible terminal을 구현하지 않았으므로 `CPP-COMP-004`를 종결하지 않는다.
12. `CPP-EXEC-004`에서 raw routed one-way send가 `bool` 하나로 backpressure와 disconnected를
    합치던 경로를 분리했다. `raw_route_port_t::send_result`가 native `submit_error_t::result()`와
    non-blocking false를 각각 보존하고, raw mesh Actor/Spot result API와 public host가 이를 그대로
    반환한다. Local Actor dispatch는 application message budget 초과를 `backpressured`로 거부한다.
    따라서 기존 `async_submit_runtime`의 send-timeout 재시도와 deadline 변환이 실제 backpressure
    결과를 받을 수 있다. `test_cpp_framework_messaging`, `execution`, `m6b`, `m6c`, target contract,
    ToActorMessaging 전체와 ChannelEgressRouting CH-E2E-05가 통과했다. 같은 런타임 포화 뒤
    `DeadlineExceeded`를 caller가 관찰하는 독립 process assertion과 모든 one-way surface matrix가
    남아 있으므로 `CPP-EXEC-004`는 PARTIAL로 유지한다.
13. `CPP-EXEC-004`의 남은 직접 결과 손실 지점을 추가로 정리했다. `spot_context_t`의 mesh
    send와 `spot_node_runtime_t::send_spot_mesh_parts`가 `submit_result_t`를
    `internal_failure`로 바꾸지 않고 `capacity_exceeded`·`unavailable`·`shutting_down` 등
    기존 Framework error kind로 전달한다. `spot_context_t::publish_erased`와
    `spot_handle_t::publish`도 native publish 및 peer 전송 결과를 보존하고 local application
    budget을 먼저 검사한다. 전체 build, raw-route/operation/execution/messaging/M6C/target
    contract CTest, ToActorMessaging 전체, ChannelEgressRouting CH-E2E-05, packaged clean
    consumer가 통과했다. 동일한 런타임 포화 뒤 `DeadlineExceeded`를 관찰하는 독립 process
    assertion과 모든 one-way surface matrix가 아직 없어 이 변경으로 항목을 종결하지 않는다.
14. `CPP-EXEC-003`의 coroutine self-wait와 cross-Spot actor-create gate 우회를 보강했다.
    Ambient continuation snapshot이 Actor execution context를 함께 전달하므로 `co_await` 뒤에도
    동일 Actor request가 `InvalidOperation`으로 거부된다. Actor create callback은 source와 target
    Spot이 같은 serial turn을 공유할 때만 inline 실행하고, 서로 다른 Spot이면 target lane을
    예약한다. `test_cpp_framework_m6b_runtime`의 coroutine 회귀를 포함한 build·CTest가 통과했다.
    설치 package와 process-level 오류 kind 증거가 남아 있어 `CPP-EXEC-003`은 PARTIAL로 유지한다.
15. `CPP-OWN-003`의 claim 단계에서 이미 ingress 검증을 통과한 frozen summary를 pending delivery가
    보관하도록 바꿨다. `try_claim()`은 canonical queue bytes를 다시 `decode_frozen_record()`하지
    않고 summary의 application payload를 이동해 delivery를 만든다. Queue가 relocation을 위해
    보관하는 canonical bytes와 claim delivery의 decoded payload를 별도로 소유하는 기존 경계는
    유지하되, claim마다 발생하던 canonical decode와 그 summary allocation은 제거했다. M6B·M6C·
    messaging와 target contract가 통과했다. Ingress canonical encode 자체와 transport/process
    copy 계측은 남아 있어 `CPP-OWN-002/003`을 종결하지 않는다.
16. `CPP-OWN-002`의 codec 내부에서도 typed application record를 만들자마자 다시
    `decode_frozen_record()`하던 왕복을 제거했다. `encode_frozen_application_record()`가 typed
    fields에서 summary를 직접 만들고 canonical bytes만 relocation queue에 넘긴다
    (`cpp/framework/src/runtime/protocol/service_wire_codec.cpp:3221-3323`). 기존 decoder와
    summary가 달라지지 않는 service-wire fixture, M6B/M6C와 target contract가 통과했다.
    이어서 `raw_stateful_dispatch_t::ingest`가 typed record와 summary를 각각 queue와 pending
    delivery에 이동하고, `maintenance_runtime`만 relocation snapshot/replay 시 canonical bytes를
    만들도록 바꿨다(`raw_stateful_dispatch.cpp:425-456`, `maintenance_runtime.cpp:659-669,1818-1828`).
    따라서 정상 ingress의 canonical encode allocation은 제거됐다. 설치 package와 실제 process
    allocation/copy 계측이 남아 있어 `CPP-OWN-002`는 PARTIAL·검증 중으로 재분류한다.
17. `CPP-OWN-007`의 포화 경로에서 canonical envelope를 만들기 전에 application mailbox
    capacity를 advisory preflight하도록 추가했다. `stateful_object_runtime_t::application_admission()`은
    동일한 message·byte·relocation hold 한도를 확인하고, 실제 `enqueue()`가 mutex 안에서 다시
    최종 검사한다(`stateful_object_runtime.hpp:270-277`, `stateful_object_runtime.cpp:731-778`,
    `raw_stateful_dispatch.cpp:426-449`). 따라서 이미 포화된 owner는 canonical encode 전에
    거부되고, 경합으로 상태가 바뀌면 기존 enqueue 결과가 우선한다. M6B/M6C, messaging와
    target contract가 통과했다. Header/authority decode와 typed frozen construction은 아직
    preflight 전에 수행되므로 `CPP-OWN-007`은 PARTIAL·중으로 유지한다.
18. SubmitAdmission의 `receiver_gate.py`가 HTTP health 이후에도 target mesh listener가 아직
    bind되지 않은 순간의 backend `ECONNREFUSED`를 즉시 client disconnect로 바꾸던 process-fixture
    race를 확인했다. bounded 5초 reconnect를 추가하고 Python syntax gate를 통과시켰지만,
    `SA-E2E-08` 재실행에서도 실제 remote direct submit이 `RouteNotConnected`로 끝났다.
    따라서 fixture race만으로 production route 실패를 덮지 않고, process evidence는 열린 상태로
    유지한다. 변경은 C++ E2E support와 plan 문서에만 적용했으며 공통 spec/schema는 수정하지 않았다.
19. 2026-08-08 다른 언어 구현을 기준으로 위 세 blocker 판정을 재검토했다. Node와 .NET의
    ordinary local binding은 Session owner ID를 node RID, lease generation을 node generation으로
    기본 설정하고, Java의 relocation/handoff는 명시적인 authority owner token을 기존 command
    42/44/51에 보존한다. 이 비교로 `BLOCKER-CPP-SESSION-OWNER-LEASE-SOURCE`는 spec blocker가
    아닌 기본값·explicit token 보존 conformance로, `BLOCKER-CPP-SESSION-ROUTE-ID`는 공용 wire가
    이미 표현하는 값을 C++ 내부 DTO가 누락하는 implementation gap으로 재분류했다. 375 대 374
    inventory 불일치도 C++ gate baseline drift로 재분류했다. 세 항목 모두 common spec/schema/
    generated asset 변경은 필요하지 않으며, ST-F3A·session lifecycle·inventory는 구현 또는
    후속 E2E gate가 남아 열린 상태다. 기준 commit은 §7.1의 `137f2858bf7fd29f58405893473be8e773725a93`이며,
    이번 재검토는 report 문구만 수정하고 source·spec·E2E process는 추가 변경·실행하지 않았다.
20. 2026-08-08 `CPP-OWN-002` hot path를 다시 확인했다. 정상 application ingress는
    `frozen_application_record_t`를 queue에 보관하고 summary만 pending delivery에 이동하며,
    canonical bytes는 `maintenance_runtime`의 relocation snapshot/replay 직렬화에서만 만든다.
    `test_cpp_framework_service_wire_codec`, `test_cpp_framework_m6b_runtime`,
    `test_cpp_framework_m6c_runtime`, `test_cpp_framework_target_contract`를 현재 main worktree
    후보에서 다시 실행해 4/4 통과했다. Unit regression은 delivery의 `turn.payload`가 비어 있고
    typed record와 summary가 유지되는지 확인한다. 사용자가 보류한 설치 package·process E2E와
    allocation 계측은 실행하지 않았으므로 이 항목은 PARTIAL·검증 중이다. 이번 후보는 commit하지
    않은 dirty main이며 다른 작업자의 변경은 stage·원복하지 않았다.
21. 2026-08-08 `CPP-EXEC-001`의 현재 lock 경로를 다시 대조했다. 제거된
    `actor_mailboxes`를 근거로 삼지 않고, Actor queue admission·SpotWide shared gate·callback
    sealing이 각각 별도 mutex를 사용하는 현재 source를 기준으로 판정을 갱신했다. queue completion
    뒤 인접한 두 queue lock은 하나로 합쳤고 `test_cpp_framework_execution`,
    `test_cpp_framework_m6b_runtime`, `test_cpp_framework_target_contract`가 통과했다. 그러나
    SpotWide admission의 중첩 lock과 무경합 atomic gate 부재는 남아 있으므로 source GAP을 유지한다.
    성능 계측과 process E2E는 사용자가 보류한 범위라 실행하지 않았다.
22. 2026-08-08 `CPP-OWN-007`의 admission 순서를 다시 대조했다. encoded application
    envelope에서 HWM charge를 allocation 없이 계산하는 overload를 추가하고, stateful ingress가
    route validation 뒤 mailbox admission을 먼저 수행하도록 바꿨다. 따라서 full queue에서는
    application payload vector·typed frozen body·canonical relocation bytes를 만들지 않는다.
    `test_cpp_framework_service_wire_codec`, `test_cpp_framework_m6b_runtime`,
    `test_cpp_framework_m6c_runtime`, `test_cpp_framework_target_contract`가 4/4 통과했다.
    `verify_packaged_contract.sh`도 설치 header 112개와 clean consumer 실행 `OK`로 통과했다.
    Header·route authority 조회와 process allocation 계측은 남아 있어 이 항목을 PARTIAL·검증
    중으로 유지한다.
23. 2026-08-08 `CPP-COMP-003`의 correlation 생성 순서를 다시 대조했다. public host가
    `call_id_t`를 completion table에 먼저 예약한 뒤 그 low half를 raw Mesh node/channel/Spot/Actor
    request의 correlation 입력으로 넘기도록 연결했다. raw transport는 입력 correlation을
    operation registry에 등록한 뒤 submit한다. `test_cpp_framework_m6b_runtime`,
    `test_cpp_framework_m6c_runtime`, `test_cpp_framework_target_contract`가 3/3 통과했고,
    설치 package는 clean consumer까지 통과했다. Host holding table 조회와 infrastructure
    request 전체 전환은 남아 있으므로 CPP-COMP-003은 PARTIAL·중으로 유지한다.
24. 2026-08-08 `CPP-COMP-004`의 holding table 초과 경로를 다시 대조했다. completion
    table의 `complete()`가 capacity 때문에 실패하면 해당 operation을 별도 bounded overflow
    set에 보관하고, `wait_for_completion()`이 이를 `CapacityExceeded`로 반환하도록 수정했다.
    timeout tombstone과 overflow set은 stop 시 함께 정리한다. M6B/M6C와 target contract가
    3/3 통과했지만 65,536개 saturation을 직접 채우는 스트레스 gate는 실행하지 않았으므로
    CPP-COMP-004는 PARTIAL·검증 중으로 유지한다.
25. 2026-08-08 이번 작업 종료 전 source 변경 후 gate를 다시 실행했다. `test_cpp_framework_service_wire_codec`,
    `test_cpp_framework_m6b_runtime`, `test_cpp_framework_m6c_runtime`,
    `test_cpp_framework_target_contract`가 4/4 통과했고, 설치 package 112개 header와 clean
    consumer를 확인하는 `verify_packaged_contract.sh`도 `PASS`했다. 사용자가 보류한 추가
    process E2E와 65,536개 completion saturation 스트레스는 실행하지 않았으며, 이 결과만으로
    관련 PARTIAL을 CLOSED로 올리지 않는다.
26. 2026-08-08 `CPP-EXEC-001`의 Actor queue lookup을 다시 대조했다. 기존 Actor packet마다
    node mutex로 queue map을 찾던 경로를 lifecycle 시점에 publish하는 copy-on-write snapshot과
    atomic load로 바꾸고, queue 생성·삭제 경계에서만 node mutex를 사용한다. `test_cpp_framework_execution`,
    `test_cpp_framework_m6b_runtime`, `test_cpp_framework_target_contract`가 3/3 통과했다.
    SpotWide admission의 `callback_mutex`와 queue lock, 무경합 close gate 부재는 남아 있으므로
    CPP-EXEC-001은 GAP으로 유지한다.
27. 2026-08-08 `CPP-LAYER-003`의 Actor dispatch 이름 경로를 다시 대조했다. actor queue
    admission에서 진단 문자열을 다시 연결하지 않고 기존 work name을 이동해 전달하도록 수정했다.
    `test_cpp_framework_execution`, `test_cpp_framework_m6b_runtime`,
    `test_cpp_framework_target_contract`가 3/3 통과했으며, per-turn `shared_ptr`와 중첩
    `std::function` 캡처는 남아 PARTIAL·중으로 유지한다.
28. 2026-08-08 Actor queue snapshot과 work-name 경로를 반영한 최신 build에서
    `test_cpp_framework_service_wire_codec`, `test_cpp_framework_m6b_runtime`,
    `test_cpp_framework_m6c_runtime`, `test_cpp_framework_target_contract`가 4/4 통과했다.
    이어서
    `verify_packaged_contract.sh framework/languages/cpp/build`를 실행해 설치 header 112개와
    clean consumer 실행 `OK`, `verify_packaged_contract: PASS`를 확인했다. 이 package gate는
    process E2E나 allocation 계측을 대체하지 않으므로 관련 PARTIAL 상태는 유지한다.
29. 2026-08-08 `CPP-COMP-002`를 common spec의 terminal-once·operation identity 요구와
    실제 owner 경계에 대조했다. transport deadline, replay dedup, public task continuation,
    caller submit/cancellation은 서로 다른 결과와 수명을 소유하므로 내부 completion primitive를
    하나로 합칠 근거가 없었다. `test_cpp_framework_operation_registry`가 통과했고 M6B/M6C와
    target contract도 terminal 결과를 중복 확정하지 않는 경로를 통과하므로 이 항목을 CLOSED로
    재분류했다. 이 판정은 public contract나 공용 schema를 변경하지 않는다.

### 7.3 실행한 gate와 결과

통과한 주요 gate는 다음과 같다.

- C++ build: `cmake --build framework/languages/cpp/build --target zlink_framework ... -j2`
- focused CTest는 service-wire codec, authority-key codec, operation registry, raw-route contract,
  execution, M6B, M6C, messaging와 target contract를 포함해 최신 8/8 통과.
- `framework/languages/cpp/e2e/SpotService/run_e2e.sh SM-D6` — 통과. 증거:
  `framework/languages/cpp/e2e/SpotService/logs/20260808-172604-3982434/`.
- InstanceSpot IS-E2E-05 — 통과. 증거:
  `framework/languages/cpp/e2e/InstanceSpot/logs/20260808-172816-4022130/`.
- SpotActorTransfer ST-F1~F5 combined — 통과. 증거:
  `framework/languages/cpp/e2e/SpotActorTransfer/logs/20260808-172739-4014158/`.
- `framework/languages/cpp/scripts/verify_packaged_contract.sh framework/languages/cpp/build` —
  통과. clean consumer 실행 결과 `OK`, `verify_packaged_contract: PASS`, 설치 header 112개.
- 최신 isolated package/process 재실행에서 `SubmitAdmission CPP-DISP-001`과
  `CPP-DISP-002`가 각각 `PASS`했다. 두 runner는 공식 Core runtime SHA-256
  `29a14581e19579fa6f2126ce1f9a797a08fe38d5788d52abb7e87689203f877b`와 C++ binding
  candidate의 native SHA-256이 같음을 확인했다. 증거는 각각
  `framework/languages/cpp/e2e/SubmitAdmission/logs/20260808-200412-303446/`와
  `framework/languages/cpp/e2e/SubmitAdmission/logs/20260808-200753-450383/`이다.
- `test_cpp_framework_layout_contract` — `RuntimeMonitoring/run_e2e.sh`가 build directory를
  허용된 `ZLINK_CPP_BUILD_DIR`로만 선택하고 skip-build 환경변수를 읽지 않도록 고친 뒤 통과.
- 최종 재실행에서 service-wire codec, authority-key codec, operation registry, M6B, M6C,
  raw-route contract, target contract, execution, messaging을 포함한 focused CTest 8개가
  8/8 통과했고, `test_cpp_framework_layout_contract`도 1/1 통과했다. `verify_packaged_contract.sh`도
  설치 header 112개, clean consumer 실행 `OK`와 함께 다시 `PASS`했다. 결과 파일은
  `/tmp/cpp_focused_ctest_20260808_final2.out`과 `/tmp/cpp_packaged_contract_20260808_final2.out`에
  남겼다.
- provider authority 삭제·재생성 회귀를 포함한 `test_cpp_framework_opaque_store_providers`가
  1/1 통과했다. ToActorMessaging 전체 시나리오(`--scenario=all`)도 통과했으며 process 증거는
  `framework/languages/cpp/e2e/ToActorMessaging/logs/20260808-193704-3806278/`에 남겼다.
- `test_cpp_framework_operation_registry`, `test_cpp_framework_m6c_runtime`,
  `test_cpp_framework_target_contract`, `test_cpp_framework_execution`,
  `test_cpp_framework_messaging`, `test_cpp_framework_raw_route_port_contract`가 raw send 결과
  보존과 timeout tombstone 변경 뒤 통과했다.
  ToActorMessaging 전체 최신 실행은
  `framework/languages/cpp/e2e/ToActorMessaging/logs/20260808-202358-874375/`에,
  ChannelEgressRouting CH-E2E-05 최신 실행은
  `framework/languages/cpp/e2e/ChannelEgressRouting/logs/20260808-202415-887676/`에 남겼다.
  그 뒤 `verify_packaged_contract.sh framework/languages/cpp/build`를 다시 실행해 설치 header
  112개와 clean consumer `OK`를 확인했다. focused 7개 CTest는 첫 aggregate 실행에서
  `test_cpp_framework_m6b_runtime`의 기존 deadline timing assertion이 1회 실패했지만,
  동일 binary 단독 재실행은 통과했다. 이 일회성 실패는 별도 안정성 finding으로 남기며
  변경된 one-way 결과 매핑의 회귀로 판정하지 않는다.
- frozen application summary와 mailbox admission preflight를 추가한 뒤 service-wire codec,
  M6B, M6C, messaging와 target contract CTest를 다시 실행해 4/4 통과했다. 설치 package
  `verify_packaged_contract.sh framework/languages/cpp/build`도 설치 header 112개와 clean
  consumer `OK`로 통과했고, ToActorMessaging 전체는
  `framework/languages/cpp/e2e/ToActorMessaging/logs/20260808-205310-1560402/`,
  ChannelEgressRouting CH-E2E-05는
  `framework/languages/cpp/e2e/ChannelEgressRouting/logs/20260808-205727-1713266/`,
  SpotService SM-D6는
  `framework/languages/cpp/e2e/SpotService/logs/20260808-205741-1713653/`에 증거를 남겼다.
- session owner fence finding과 독립적인 topology·location regression 3개
  (`test_cpp_framework_m6a_runtime`, `zlink_cpp_framework_mesh_node_vertical_test`,
  `test_cpp_framework_store_location_resolvers`)를 2026-08-08 최신 binary로 다시 실행해
  3/3 통과했다. 이 결과는 아직 남은 C++ route propagation conformance나 공통 E2E inventory
  baseline drift를 종결하는 증거로 사용하지 않는다.
- SubmitAdmission의 기존 process selector `SA-E2E-08`, `SA-E2E-09`, `SA-E2E-14`, `SA-E2E-20`을
  최신 isolated package로 재실행했지만 첫 remote Node direct 호출이 `Submitted` 대신
  `RouteNotConnected` terminal을 받아 runner가 중단됐다. Candidate Core SHA-256은
  `29a14581e19579fa6f2126ce1f9a797a08fe38d5788d52abb7e87689203f877b`이고, 증거는
  `framework/languages/cpp/e2e/SubmitAdmission/logs/20260808-210522-1901318/`이다.
  이 실행은 SA-E2E-08/09/14/20의 완료 증거가 아니며, C++ process route readiness와 실제
  submit 사이의 연결 단절 원인을 별도 finding으로 남긴다.
- 같은 runner에서 Mesh peer가 필요하지 않은 `SA-E2E-14`를 단독 실행해
  `SA-E2E-14 PASS`와 `shared_version_reference_unchanged=0.10.1`을 확인했다. 증거는
  `framework/languages/cpp/e2e/SubmitAdmission/logs/20260808-210850-1921854/`이다.
  이 결과는 publisher one-way admission의 해당 process 경로만 증명하며, 실패한
  SA-E2E-08/09/20 또는 Config 13 전체 완료로 승격하지 않는다.

최신 source로 sample을 제외한 Framework unit/contract/perf 46개를
`ctest --test-dir framework/languages/cpp/build --output-on-failure -L framework -E '^sample_' -j1`
로 serial 실행했으며 45개가 통과하고 1개가 실패했다. 최신 결과는
`/tmp/cpp_common_inventory_20260808_final4.out`에 남겼다. `test_cpp_framework_layout_contract`는
`RuntimeMonitoring/run_e2e.sh`의 build 환경변수 사용을 제거한 뒤 재실행해 통과했다. 현재
남은 non-sample 실패는 다음 하나다.

| 테스트 | 관찰된 실패 |
|---|---|
| `test_cpp_framework_common_e2e_inventory` | 최신 serial 재실행에서도 공통 문서 375개와 C++ gate 기대값 374개가 불일치하고, Config 02/03/05/06/08/10/11/13/14의 feature-map/source ID와 292개 조건이 누락됐다. 이는 `OPEN-GATE-CPP-COMMON-E2E-INVENTORY` baseline drift와 후속 E2E 미완료 증거다. `blocked` 항목을 임의로 complete로 바꾸지 않고 사용자가 보류한 gate로 유지한다. 실행 결과는 `/tmp/cpp_common_inventory_20260808_final4.out`에 남겼다. |

sample을 포함한 52개 전체 aggregate는 다른 작업이 이미 실행 중인 C++/Node sample process와
port를 공유한 상태에서 `DeliveryDispatch` 이후 중단되어 완료 증거로 사용하지 않았다. 독립
재실행한 sample 중 `SpotService SM-D6`, `InstanceSpot IS-E2E-05`, `SpotActorTransfer ST-F1~F5`는
각각 위 log로 통과했다. `SupportChat`은 최신 binary에서도 conversation Actor 이동 중 bind가
`Remote Actor session binding did not complete successfully`로 실패했으며, 이 finding은 §7.2에
기록했다. 따라서 sample 전체 aggregate와 cross-language process gate는 여전히 미실행 또는
실패 상태다.

SupportChat은 다른 sample과 분리한 `ctest -R '^sample_smoke_sample_cpp_framework_SupportChat$'`로
2026-08-08 18:26에 다시 실행해도 같은 결과였다. `JoinConversationReq`가 실패했고 client는
`Remote Actor session binding did not complete successfully`를 기록했다. Support process에는
두 Actor의 `actor_joined_begin`·`actor_joined_complete`가 남았지만 session flow에는 해당 request의
dispatch error가 남았다. 이 결과는 port 충돌이 아니라 moving Actor initial bind lifecycle finding의
독립 재현이다. 실행 당시 CTest temporary process log는 자동 삭제되었고, persistent flow 증거는
`framework/languages/cpp/samples/SupportChat/logs/flow-{api,session,support}.log`에 남아 있다.

이 결과는 현재 session replacement focused pass를 무효화하지 않지만, Framework 전체의 필수
gate가 열려 있으므로 완료 조건을 충족하지 않는다. 다른 작업자의 실행 process와 port 충돌로
중단된 aggregate는 C++ pass로 승격하지 않았다.

### 7.4 잔여 판정과 다음 순서

현재 판정은 `NOT CLEAN / E2E-DEFERRED`다. 현재 공용 spec·schema 변경 blocker는 없다. CPP-LAYER-003/004,
CPP-EXEC-001/004, CPP-COMP-003, CPP-FOLLOW-002, CPP-ROUTE-001, CPP-LIFE-003,
CPP-TIMER-003 등 Medium 이상 finding이 남아 있고, CPP-COMP-004·CPP-OWN-007은 source 수정 뒤
검증 gate가 남아 있다. CPP-OWN-002는
source hot-path 수정 뒤 설치 package·process allocation gate가 남아 있으며, 위 aggregate
실패와 cross-language/package process gate가 미실행 또는 실패 상태다. 따라서 POSDDD 리뷰는 시작하지
않았다. 먼저 contract/runtime finding과 필수 gate를 닫고, 그 뒤 production runtime → unit test
순서로 POSDDD 리뷰를 수행하며 owner-layer regression과 aggregate gate를 반복한다. Medium 이상 0개,
미실행 필수 gate 0개, 최종 Codex Sol 결과 `CLEAN`일 때만 완료로 판정한다.

사용자 지시에 따라 SubmitAdmission·InstanceSpot 등 추가 process E2E 실행은 후속 작업으로
보류한다. 현재 확보된 process 증거와 실패 로그는 열린 gate로 유지하며, 보류를 완료로 간주하지
않는다. E2E를 제외한 runtime·unit·package 검토와 남은 C++ 구현 finding 정리는 계속 진행한다.
