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

## 1. 집계

| 문서 | GAP | PARTIAL | 비고 |
|---|---:|---:|---|
| 01-layering | 2 | 5 | relocation 재시도 불가가 핵심 |
| 02-serialization | 2 | 2 | 락 횟수, self-wait 오류 종류 |
| 03-progress-isolation | 2 | 2 | HOL 블로킹, `std::terminate` |
| 04-completion | 2 | 2 | 문자열 분류, 완료 방식 난립 |
| 05-relocation-continuity | 1 | 2 | 오류 종류 축약 |
| 06-routing-and-cache | 1 | 2 | 수동 피어 fence (JVM-TOPO-001의 C++ 판) |
| 07-dispatch-loop | 2 | 3 | tick 통계 무한 누적, timer 계약 불일치 |
| 08-object-lifecycle | 1 | 2 | generation 필터링과 Ready owner loss 판정 |
| 09-session-binding | 1 | 2 | 세션 스왑 핸드셰이크 부재 |
| 10-liveness-and-state | 0 | 2 | 대체로 충실 |
| 11-message-ownership | 5 | 4 | 복사/보관 규율 전반 미준수 |
| 12-service-wire-protocol | 5 | 2 | 스토어 키 포맷, json-v1 프로파일 |
| **internals 소계** | **24** | **30** | 기존 `CPP-TIMER-002`를 정식 계약 위반으로 재분류하고 기존 누락 집계 2건을 교정 |
| C++ exact public interface | 6 | 0 | diagnostics 2, object query, STREAM timeout, ClientServer role, HTTP builder |
| **합계** | **30** | **30** | 코드만으로 확인할 수 없는 package·process 조건은 제외 |

---

## 2. 우선 대응 항목 (심각도 상)

즉시 수정 계획이 필요한 항목. 프로세스 종료, 이중 소유, 크로스 런타임 상호운용 파괴, 복구 불가 상태에 해당한다.

| ID | 요약 | 문서 |
|---|---|---|
| CPP-DISP-001 | 디스패치 executor 포화 시 예외가 pump 스레드를 탈출해 `std::terminate` — 프로세스 강제 종료 | 03 §6 |
| CPP-DISP-002 | 한 Spot 큐 포화가 노드 전체 애플리케이션 수신을 head-of-line 블로킹, Request가 즉시 실패하지 않고 타임아웃까지 대기 | 03 §5 |
| CPP-SESS-001 | 원격 세션 bind 시 이전 소유자 통지·정리 확인 절차가 없어 두 노드가 동시에 같은 Actor의 세션을 소유 가능 | 09 §3 |
| CPP-WIRE-001 | Location Store 권한 키를 `zla1:…` 포맷으로 통합했으며 package와 cross-language Store 검증 진행 중 | 12 §1 |
| CPP-RELOC-001 | relocation의 blocked/target_unavailable 결과가 terminal로 영구 저장되어, 일시 실패 후 재시도가 프로세스 재시작 전까지 불가능 | 01 §3 |
| CPP-TOPO-001 | 수동 설정 피어에 Location Store descriptor의 admission fence(generation/보안 identity)가 설치되지 않음 — 이미 종결된 `JVM-TOPO-001`과 동일 계열 결함의 C++ 판 | 06 §1.1 |
| CPP-CONTRACT-DIAG-001/002 | diagnostics level과 public export가 C++ exact interface와 다르고, 제거 대상 observer·raw DTO·file/label 설정이 설치 header에 남아 있음 | C++ interface 08 |
| CPP-CONTRACT-QUERY-001 | Actor·Spot exact lookup과 bounded object page public surface가 없음 | C++ interface 07 |
| CPP-CONTRACT-STREAM-001 | STREAM one-way send timeout 구현 완료, package·process 검증 진행 중 | C++ interface 03 |

### CPP-DISP-001 — executor 포화 → `std::terminate`
mesh 디스패치 스레드는 throwing `submit`으로 애플리케이션 작업을 넘기는데, `offload_executor_t::submit`은 내부 큐(4096) 포화 시 `std::runtime_error`를 던진다. 둘러싼 `catch (...)`는 정리 후 재던지고, pump 스레드 람다에는 try/catch가 없어 `std::thread` 본체를 탈출한 예외가 `std::terminate`를 호출한다. 서로 다른 owner의 in-flight 디스패치가 4096개를 넘는 순간 재현 가능하다. 스펙 03 §6은 "한도 초과를 조용히(또는 파괴적으로) 처리하지 말고 관찰 가능한 거부 결과를 내라"고 결정했다.
- 증거: `cpp/src/runtime/mesh/mesh_node_host_service.cpp:2706, 2804-2812, 2574-2836`, `cpp/src/runtime/dispatch/offload_executor.cpp:50-55`
- 구현 checkpoint `1cd08afc2d`: MeshNode pump와 local send가 throwing `submit`을 호출하지 않는다. Queue가 포화되면 원격 Request에는 `CapacityExceeded` envelope를 즉시 reply하고, local send에는 `backpressured`를 반환한다. Admission accounting과 mailbox claim도 같은 분기에서 terminal 처리한다. Executor saturation 회귀, `test_cpp_framework_target_contract`, `test_cpp_framework_host_lifecycle`는 통과했다. Idle-eviction fixture의 음수 timestamp를 수정한 checkpoint `af11dabeac` 이후 `test_cpp_framework_execution` 전체도 통과했다. 실제 pump 포화 process E2E는 아직 남아 있다.

### CPP-DISP-002 — 노드 전체 head-of-line 블로킹
admitted 피어의 애플리케이션 레코드가 대상 owner의 mailbox 예산(1024건/64 MiB)에 들어가지 못하면 레코드를 `_pending_received` 단일 슬롯에 보관하고 `backpressured`를 반환하는데, 이 보관 레코드가 소진될 때까지 `pump_one`은 **노드 전체의 신규 애플리케이션 수신을 중단**한다. 결과적으로 (1) 포화된 Spot을 향한 원격 Request가 `Unavailable`/`CapacityExceeded`로 즉시 실패하지 않고 호출자 타임아웃까지 대기하고, (2) 느린 Spot 하나가 노드의 다른 모든 Spot 인바운드를 막는다. 스펙 03 §5는 Request 계열 즉시 실패를 요구하고, 수신 정지는 프로세스 전역 pending-byte HWM에만 허용한다.
- 증거: `cpp/src/runtime/mesh/raw_mesh_node_owner.cpp:1814-1829, 1984-1996`, `cpp/src/runtime/mesh/service_mailbox.cpp:63-94`

### CPP-SESS-001 — 세션 스왑 핸드셰이크 부재
스펙 09 §3의 결정: 이미 다른 곳에 연결된 Actor를 새 세션에 bind할 때, 새 소유자는 새 연결을 등록하고 → 이전 소유자에게 통지하고 → 이전 소유자의 정리 확인을 기다린 뒤 → bind 완료를 응답한다. C++ 원격 bind 경로는 이 중 아무것도 하지 않는다: `actor_bound_session_bind_route_request_t` 핸들러가 `replace_existing=true`로 게이트웨이 라우트를 덮어쓰고 즉시 `accepted=true`를 응답하며, 이전 세션 소유 노드에는 어떤 메시지도 보내지 않는다. 크로스 노드 스왑 시 이전 노드의 `stream_session_registry_t`에 살아있는 바인딩이 남아 두 세션 소유자가 동시에 같은 Actor로 인바운드를 admit할 수 있다(스테일 트래픽은 generation/fence 검사로 기회적으로만 걸러짐). 로컬 노드 스왑은 레지스트리가 원자적으로 교체하므로 안전하다.
- 증거: `cpp/src/runtime/spots/spot_route_internal_dispatcher.cpp:199-216`, `cpp/src/runtime/actors/actor_gateway_runtime.cpp:1189-1252`, `cpp/src/runtime/mesh/mesh_node_runtime.cpp:2498-2592`, `cpp/src/runtime/streams/stream_host_service.cpp:1654-1660`

### CPP-WIRE-001 — 스토어 권한 키 포맷 불일치
스키마가 고정한 키 포맷 `zla1:<a|s>:<byte-length>:<percent-encoded-id>`는 **읽기 시 3종 probe 중 하나**로만 구현되어 있고, 권한 쓰기/CAS 어댑터는 `"1:" + key` / `"2:" + key` 레거시 포맷으로 기록한다. 다수의 런타임 읽기 지점도 레거시 포맷을 직접 사용한다. C++ 런타임이 기록한 권한 행은 스키마 포맷 아래에 존재하지 않으므로, 같은 스토어를 공유하는 타 언어 런타임과의 상호운용이 깨진다.
- 증거: `cpp/src/runtime/stateful/public_store_adapters.hpp:345-351` (쓰기), `cpp/src/runtime/locations/store_location_resolvers.hpp:369-397` (3종 probe), `cpp/src/runtime/host/app.cpp:99`, `cpp/src/runtime/mesh/mesh_node_host_service.cpp:109, 1111`, `cpp/src/runtime/actors/actor_client.cpp:589`

### CPP-RELOC-001 — relocation 영구 차단
`run_shared_relocation`의 `complete()`는 `blocked/target_unavailable`을 포함한 **모든** 결과에 `operation.terminal = true`를 설정하고, `relocation_operation`은 어디에서도 리셋되지 않는다. 이후 `relocate()` 호출은 저장된 blocked 결과를 영원히 반환한다. 스펙 01 §3은 "거부된 결과는 저장하지 않으며, 재요청 시 처음부터 다시 검사한다"고 결정했다. 연관 gap: preflight와 worker 모두 대상 조회를 1회만 수행해(스펙이 요구하는 "설정된 시간까지 대상 정보 전파 대기" 없음) 전파 경합 중의 relocation이 스퓨리어스하게 거부되고, 위 문제와 결합되면 영구 거부가 된다(→ CPP-RELOC-002, §3.1).
- 증거: `cpp/src/runtime/host/app.cpp:2852-2854, 2959-2960` (terminal 저장), `2741-2758, 3072-3077, 3291-3296` (단발 조회)
- 구현 checkpoint `9fc3179a68`: `relocated` 결과만 terminal로 보존하고, `blocked` 결과는 waiter 완료 뒤 operation을 다시 시작 가능한 상태로 되돌린다. 다음 호출은 이전 worker thread를 join한 뒤 preflight부터 다시 실행한다. `test_cpp_framework_target_contract`와 기본 `test_cpp_framework_host_lifecycle`는 통과했다. 실제 재시도 process 회귀를 추가해 실행하는 과정에서 기존 optional fixture가 readiness payload를 보내기 전에 `No serializer is registered for this payload type`으로 실패했으므로, serializer owner gap을 우회하지 않고 재시도 process 증거를 보류한다.

### CPP-TOPO-001 — 수동 피어 admission fence 미설치
자동 연결 루프는 descriptor의 `lifecycle_generation`/`security_identity`를 `expect_peer`/`connect_peer`로 설치하지만, **수동 연결 목록에 있는 엔드포인트의 descriptor는 명시적으로 건너뛴다**. 수동 피어 등록 경로는 endpoint + 선택적 RID만 받으므로 fence가 설치될 길이 없다. 해당 엔드포인트의 스테일/대체 노드가 descriptor fence로 거부되지 않는다. 이는 JVM에서 이미 수정·종결된 `JVM-TOPO-001`과 동일 계열 결함이다.
- 증거: `cpp/src/runtime/locations/location_auto_connect_host_service.hpp:161-164, 182-184`, `cpp/src/runtime/mesh/mesh_node_runtime.cpp:432-440`, `cpp/src/runtime/channels/route_channel_registration.cpp:53-77`

### 2.1 의사결정 반영 뒤 추가된 public contract gap

다음 여섯 항목은 internals 문서의 구현 구조 차이가 아니라 정식 C++ exact interface와 설치되는 public header가 직접 다른 경우다. 따라서 source에 비슷한 내부 기능이 있는지만으로 충족 판정을 내릴 수 없다.

| ID | 분류 | 계약과 현재 구현의 차이 |
|---|---|---|
| CPP-CONTRACT-DIAG-001 | GAP·상 | exact interface는 `off/errors/normal/detailed` 네 level을 고정하지만 public header는 `off/errors_only/key_transitions/verbose/diagnostic` 다섯 값을 export한다. 증거: `common/spec/server/languages/cpp/interfaces/08-monitoring.ko.md:200-229`, `cpp/include/zlink/framework/contracts/dispatch/execution.hpp:25-32` |
| CPP-CONTRACT-DIAG-002 | GAP·상 | exact interface가 제외한 raw flow/error DTO, observer callback, file path와 label 설정을 public header가 계속 export한다. 제거 전에 application logger provider를 통한 structured record와 provider-failure 격리 process E2E를 먼저 확보해야 한다. 증거: `common/spec/server/languages/cpp/interfaces/08-monitoring.ko.md:230-235`, `cpp/include/zlink/framework/contracts/dispatch/execution.hpp:34-185, 187-247` |
| CPP-CONTRACT-QUERY-001 | GAP·상 | exact interface의 Actor·Spot exact lookup, object state와 bounded object page 타입·method가 public header에 없다. 기존 status/topology/service summary 조회는 이 계약을 대신하지 않는다. 증거: `common/spec/server/languages/cpp/interfaces/07-location-store.ko.md:360-419`, `cpp/include/zlink/framework/contracts/locations/runtime_query.hpp:10-20` |
| CPP-CONTRACT-STREAM-001 | GAP·검증 중 | 구현 checkpoint `eefcda189d`에서 `stream_send_call_t::timeout(...)`을 추가하고, Core STREAM writer가 기록한 socket admission timeout을 호출별 값으로 더 짧게 제한하도록 연결했다. `1..INT_MAX` 범위를 벗어난 값은 modifier에서 거부한다. Owner-layer 회귀는 20 ms 제한이 1초 socket 기본값을 줄이는지, 만료 뒤 재시도하지 않는지, send-ready 신호 뒤 거부된 시도만 한 번 재제출하고 성공 뒤 추가 신호로 replay하지 않는지 확인한다. `test_cpp_framework_contract_headers`, `test_cpp_framework_stream_framework`, `test_cpp_framework_target_contract`가 통과했다. 설치 package의 clean-consumer compile과 실제 Core STREAM backpressure process E2E가 남아 있어 아직 종결하지 않는다. |
| CPP-CONTRACT-ROLE-001 | GAP·검증 중 | 구현 checkpoint `4b741bc692`에서 Client runtime record가 없으면 target 대기 전에 `NotConfigured`를 반환하고, Client role은 있지만 ready target이 없는 경우만 `NotFound`를 유지하도록 분리했다. Server-only host의 public `channel_client_t::send()`가 정확한 오류 kind를 반환하는 회귀와 `test_cpp_framework_target_contract`가 통과했다. Cross-language process E2E의 오류 kind assertion이 남아 있어 아직 종결하지 않는다. |
| CPP-CONTRACT-HTTP-001 | GAP·검증 중 | 구현 checkpoint `14c5f04f03`에서 `http_options_builder_t::snapshot()`과 `validate()`를 private으로 옮기고 host의 `app_t`와 `zlink_framework_options_t`만 접근하도록 제한했다. Application surface에서 두 method가 보이지 않는 negative compile assertion을 추가했고 `test_cpp_framework_contract_headers`, `test_cpp_framework_app_host`가 통과했다. 설치 package의 clean-consumer compile이 남아 있어 아직 종결하지 않는다. |

### 2.2 의사결정 검토에서 gap으로 추가하지 않은 항목

- STREAM 인증은 application callback의 책임이다. Framework public auth gate가 없다는 사실은 gap이 아니다(DEC-07).
- relocation의 target 선택, 일부 Actor만 옮기는 modifier와 public packet sequence/observer는 계약에서 제외됐다. 이 API들이 없다는 이유로 gap을 추가하지 않는다(DEC-10, DEC-12).
- logical disconnect는 기존 unbind 의미를 강화하는 대상이다. 별도 `Unbind` 이름을 추가하지 않는다(DEC-11).
- Message Follow 중복 억제의 내부 알고리즘은 언어별 재량이다. 안전 결과를 만족하는 한 특정 suppression helper가 없다는 이유로 gap으로 분류하지 않는다(DEC-13).
- C++ HTTP `snapshot()`/`validate()`의 public 노출은 gap이지만, 별도 public snapshot type을 새 계약으로 만드는 방식으로 해결하지 않는다(DEC-14).
- participant application state는 source에서 64 MiB 상한과 `StateIncompatible` 경로를 확인했다. 다만 경계값 process E2E를 실행하지 않았으므로 완료 증거로 승격하지 않는다(DEC-17).

---

## 3. 문서별 상세

### 3.1 01-layering (레이어 경계와 식별자)

| ID | 분류 | 요약 |
|---|---|---|
| CPP-RELOC-001 | GAP·검증 중 | §2 참조 — terminal state와 worker 회수는 수정됐고 재시도 process 증거가 남아 있음 |
| CPP-RELOC-002 | GAP·중 | 대상 정보 전파 대기 없이 단발 조회로 `target_unavailable` 판정 (§2의 CPP-RELOC-001 항목 참조) |
| CPP-LAYER-001 | PARTIAL·하 | 식별자 타입화가 절반만: `node_rid_t`/`actor_id_t`만 전용 타입이고 `spot_id_t`는 `std::string` alias, mesh/채널 이름은 평문 문자열 — 스펙이 명명한 안티패턴 그대로. 증거: `cpp/include/zlink/framework/contracts/spots/spot_identity.hpp:24` |
| CPP-LAYER-002 | PARTIAL·검증 중 | 구현 checkpoint `c2bc713c99`에서 진행 중 runtime call 식별자를 `call_id_t`로 바꾸고 내부 파일도 `call_id.hpp`로 옮겼다. 공개 Actor Join `OperationId`를 운반하는 `wire_operation_id_t`는 별도 strong type으로 정의해 같은 128-bit 표현을 쓰더라도 call ID와 암시적으로 대입되지 않는다. Foundation registry, host completion, mesh·ClientServer transport와 creation call site는 call 용어로 통일했고 compile-time assertion과 target source gate로 두 타입의 재결합을 막았다. 전체 `test_cpp_framework_operation_registry`, `test_cpp_framework_service_wire_codec`, `test_cpp_framework_m6a_runtime`, `test_cpp_framework_m6b_runtime`, `test_cpp_framework_m6c_runtime`, `zlink_cpp_framework_mesh_node_vertical_test`, `test_cpp_framework_target_contract`가 통과했다. 설치 package와 cross-process Actor Join completion 검증이 남아 있어 아직 종결하지 않는다. |
| CPP-LAYER-003 | PARTIAL·중 | 구현 checkpoint `ad335148f0`에서 handler와 deferred Join completion을 이미 직렬화하는 `actor_execution_queues`를 단일 owner로 유지하고, handler 실행 전체를 다시 잠그던 `actor_mailboxes` map·per-Actor mutex·수명 정리 코드를 제거했다. Deferred barrier ordering 회귀를 포함한 전체 `test_cpp_framework_execution`, `test_cpp_framework_m6b_runtime` 2회, `test_cpp_framework_target_contract`가 통과했다. 메시지당 work-name 문자열 연결, turn당 `shared_ptr` 다수와 중첩 `std::function` 캡처는 남아 있으므로 이 항목은 아직 종결하지 않는다. |
| CPP-LAYER-004 | PARTIAL·중 | 스트림 공개 계약에 core 바인딩 타입 누출: `stream.hpp`의 `compress/decompress/write_packet/reply_packet` 시그니처가 `zlink::message_t`를 노출 — 바인딩 메시지 타입 변경이 곧 공개 API 변경이 됨. 증거: `cpp/include/zlink/framework/contracts/streams/stream.hpp:103-104, 215-216` |
| CPP-LAYER-005 | PARTIAL·검증 중 | 구현 checkpoint `9ffc2af6ab`에서 teardown 결과를 최종 확정한 직후 `completion_admission->stop()`을 먼저 실행하고, 그 다음 termination state와 waiter 결과를 공개하도록 순서를 바꿨다. 따라서 terminal 결과를 관측한 뒤 새 completion이 admit되는 창이 없다. 순서를 고정하는 `test_cpp_framework_target_contract`와 전체 `test_cpp_framework_app_host`가 통과했다. Package host와 shutdown 중 completion 경합 process 검증이 남아 있어 아직 종결하지 않는다. |

만족 항목(요약): 책임 그래프/wrapper 금지/shutdown 순서·우선순위/동종 중복 작업 병합/등록 시점 검증/재시작 안전 호출 식별자 등은 충실. 성능 측정 항목은 PerfTests 실행 필요.

### 3.2 02-serialization (Spot·Actor 실행 직렬화)

| ID | 분류 | 요약 |
|---|---|---|
| CPP-EXEC-001 | GAP·중 | §4 "SpotWide 메시지 1건 처리 시 락 2회 미만" 위반: 노드 `recursive_mutex` + actor 큐 mutex(enqueue) + offload executor mutex + spot 큐 mutex + `complete_one` 내 큐 mutex 2회×2큐 + per-actor `actor_mailboxes` mutex — 스펙이 경고한 메시지당 이중 락 처리량 한계의 배수 형태. 증거: `cpp/src/runtime/spots/spot_runtime.cpp:2549, 5646`, `cpp/src/runtime/execution/serial_execution_queue.cpp:393, 793, 841` |
| CPP-EXEC-002 | GAP·검증 중 | 구현 checkpoint `fdb5173042`에서 Actor self-request와 같은 Spot execution gate를 기다리는 Actor·Spot request의 오류를 모두 `InvalidOperation`으로 통일했다. 제출 횟수가 0인지와 정확한 오류 kind를 확인하는 `test_cpp_framework_m6b_runtime`이 통과했다. Cross-language process E2E의 오류 kind assertion이 남아 있어 아직 종결하지 않는다. |
| CPP-EXEC-003 | PARTIAL·상 | self-wait 가드가 `thread_local`로 핸들러 turn의 동기 구간에만 성립 — `co_await` 재개 후에는 자기-요청이 탐지되지 않아 거부 대신 타임아웃까지 데드락. 별건으로 `try_create_actor`의 `caller_owns_source_turn` 분기가 소스 Spot과 대상 Spot이 다를 때 대상(Entry) Spot의 gate를 우회한 채 `on_create_actor`를 인라인 실행. 증거: `cpp/src/runtime/execution/serial_execution_queue.cpp:153-166`, `cpp/src/runtime/spots/spot_runtime.cpp:3477-3500` |
| CPP-EXEC-004 | PARTIAL·중 | Pitfall 2 큐 포화 계열 분리 미완: send/one-way의 "같은 런타임은 send 타임아웃까지 대기 후 `DeadlineExceeded`" 행이 Spot/Actor 대상에 미구현 — `actor_send`가 `actor_request`와 동일하게 즉시 `capacity_exceeded`. 증거: `cpp/src/runtime/spots/spot_runtime.cpp:2663-2673, 2765-2768` |

만족 항목(요약): per-actor 큐→공유 gate 구조, PerActor 모드, 타이머 lane, 2-lane FIFO 이중 한도, lifecycle 우선+burst 8+yield debt, 앞끼워넣기 금지, 실행 자원 비비례 등 핵심 직렬화 구조는 충실.

### 3.3 03-progress-isolation (앱/인프라 실행 분리)

| ID | 분류 | 요약 |
|---|---|---|
| CPP-DISP-002 | GAP·상 | §2 참조 — HOL 블로킹 + Request 즉시 실패 미구현 |
| CPP-DISP-001 | GAP·검증 중 | §2 참조 — source와 owner-layer saturation 회귀 및 전체 `test_cpp_framework_execution`은 통과했고 실제 pump 포화 process E2E가 남아 있음 |
| CPP-DISP-003 | PARTIAL·검증 중 | 구현 checkpoint `807a87896d`에서 미승인 peer의 bounded application queue가 포화되면 Request header의 correlation과 transport request sequence를 읽어 기존 reply 경로로 `workerQueueFull` terminal을 즉시 반환하도록 수정했다. one-way 메시지는 bounded drop을 유지하며 trace에서 terminal reply 성공과 drop을 구분한다. 실제 transport request 1,025개를 admission 전에 전송해 앞의 1,024개는 보류되고 마지막 Request는 timeout 전에 terminal reply를 받는 회귀를 추가했다. 전체 `test_cpp_framework_m6b_runtime` 2회와 `test_cpp_framework_target_contract`가 통과했다. 설치 package와 실제 양방향 handshake 경합 process 검증이 남아 있어 아직 종결하지 않는다. |
| CPP-DISP-004 | PARTIAL·검증 중 | 구현 checkpoint `7f2bf358fe`에서 caller task가 worker dequeue 시점에 성공으로 끝나는 기존 경계는 유지하되, 이후 publisher 반환 실패와 모든 예외를 fallback log와 누적 counter로 남기도록 executor를 수정했다. 표준 Spot publish는 fallback과 중복 기록하지 않고 `route_mesh_channel/publish/drop` structured event에 packet·channel·topic과 예외를 기록한다. Generic `publish_call_t`의 완료 후 실패 counter 회귀, structured observer 필드 회귀, 전체 `test_cpp_framework_messaging`, `test_cpp_framework_execution`, `test_cpp_framework_target_contract`가 통과했다. 설치 package와 실제 RouteMesh publish 실패를 사용하는 process 검증이 남아 있어 아직 종결하지 않는다. |

만족 항목(요약): 실행 영역 2분할(펌프 스레드/앱 executor), 2도메인 mailbox, 3중 한도 공존, 관찰 비차단, send 계열 backpressure 절차, 수신 HWM의 앱 한정 정지 등은 충실.

### 3.4 04-completion (완료 확정)

| ID | 분류 | 요약 |
|---|---|---|
| CPP-COMP-001 | GAP·검증 중 | 구현 checkpoint `1db9758177`에서 Actor 이동 중 `Unavailable`에 내부 `actor_transfer_in_progress` origin을 부여하고, error envelope metadata를 통해 typed origin을 보존하도록 바꿨다. 재시도는 `what()` 문구 대신 이 origin만 확인한다. Native 예외는 `std::system_error`의 transport `errc`만 `Unavailable`로 분류하고 다른 `std::exception`은 `InternalFailure`로 처리하므로 `not connected`, `stale`, `errno=113` 문자열 검색이 남지 않는다. 실제 `channel_reply_writer_t`를 거친 origin roundtrip, 전체 `test_cpp_framework_messaging`, `test_cpp_framework_execution`, `test_cpp_framework_m6b_runtime`, `test_cpp_framework_target_contract`가 통과했다. 설치 package와 Actor 이동 중 request 재시도 process E2E가 남아 있어 아직 종결하지 않는다. |
| CPP-COMP-002 | GAP·중 | §1/§7 "런타임 내 완료 확정 방식은 하나" 위반: 최소 4가지 완료 메커니즘(`operation_registry_t` take, `exactly_once_table_t` 2회, `task_shared_state_t` first-write-wins, `pending_operation_state_t` atomic flip + 채널 bool/cv + `submit_once_t`)이 공존하고 mesh request 1건이 3가지를 체이닝. 각각은 claim-once로 올바르나 새 경로가 따를 단일 규칙이 없음 |
| CPP-COMP-003 | PARTIAL·중 | §3: 완료 상관값을 submit의 **입력**으로 받는 구조가 미구현(전 계층 out-parameter). 응답 소실은 계층별 선등록으로 막혀 있으나, 스펙이 제거하려 한 holding-slot 체인(`_completions` → `_completed_operations` → cv 대기)의 완료당 추가 맵 조회 비용은 그대로. 증거: `cpp/src/runtime/stateful/public_host_runtime.cpp:2474-2495`, `cpp/src/runtime/mesh/mesh_node_runtime.cpp:2675-2708, 2773-2790` |
| CPP-COMP-004 | PARTIAL·상 | holding-slot 규율 절반 위반: mesh 계층 `_completed_operations`가 **무한**(capacity 0 = 무제한)이고, waiter의 cv 타임아웃 이후 도착한 terminal 레코드가 삽입된 채 노드 정지까지 삭제되지 않음 — 타임아웃된 요청마다 고아 엔트리 누수. 일부 인프라 요청 경로는 capacity성 `backpressured`를 `CapacityExceeded`가 아닌 `internal_failure`로 붕괴. 단순 capacity 제한은 completion 도착과 waiter 등록 사이의 경쟁에서 응답을 잃게 하므로, CPP-COMP-003의 correlation 선등록 전환과 함께 bounded holding slot·`CapacityExceeded` 완료를 구현해야 한다. 증거: `cpp/src/runtime/mesh/mesh_node_runtime.hpp:450-454`, `cpp/src/runtime/mesh/mesh_node_runtime.cpp:2868-2875, 1863-1867, 2286-2289`, `cpp/src/runtime/operations/exactly_once_table.hpp:43` |

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
| CPP-TOPO-001 | GAP·상 | §2 참조 — 수동 피어 fence 미설치 |
| CPP-ROUTE-001 | PARTIAL·검증 중 | Wire fence가 제공하지 않는 `owner_id`는 빈 값일 때 비교하지 않고, 제공되는 node·object·authority owner generation과 owner lease generation은 계속 정확히 비교하도록 수정했다. Wire fence와 같은 입력으로 actor route cache가 무효화되고 다음 조회가 store를 다시 읽는 owner-layer 회귀 test도 추가했다. 구현 checkpoint `5e31808ad3`; `test_cpp_framework_store_location_resolvers` 33/33 통과. 실제 Message Follow notice가 spot·actor call site를 거쳐 cache를 무효화하는 process E2E가 남아 있어 아직 종결하지 않는다. |
| CPP-ROUTE-002 | PARTIAL·검증 중 | 구현 checkpoint `343a9c831b`에서 direct-store fallback도 공통 `live_location_reader_t`로 authority의 owner lease와 fencing margin을 확인하고, cache 수명을 `route_cache_max_age`와 owner admission lifetime 중 짧은 값으로 제한했다. Store 시각을 읽기 전의 `steady_clock` 시각으로 절대 만료 시각을 고정하므로 변환 과정에서 lease deadline을 연장하지 않는다. 10초 cache를 설정한 두 host 회귀에서 owner admission deadline 전 첫 전송은 `ok`, deadline 뒤 두 번째 전송은 `not_found`인지 확인했고 전체 `test_cpp_framework_m6b_runtime`과 `test_cpp_framework_target_contract`가 통과했다. 설치 package와 실제 Location Store를 사용하는 process 검증이 남아 있어 아직 종결하지 않는다. |

만족 항목(요약): positive 캐시+전체 fence, 미존재/생성중/스토어 실패 비캐시, 수명 검증, follow 와이어 레코드, smooth WRR+tiebreak, 사전 계산 사이클, 직접 지정 대상 불변, publish 스냅샷 등은 충실.

### 3.7 07-dispatch-loop (수신·디스패치 루프)

| ID | 분류 | 요약 |
|---|---|---|
| CPP-TIMER-001 | GAP·검증 중 | 구현 checkpoint `80be9877f2`에서 전달 tick과 실패 기록을 각각 최근 256개로 제한했다. 300회 추가 dispatch 뒤 history 크기와 마지막 tick을 확인하는 owner-layer 회귀와 전체 `test_cpp_framework_execution`은 통과했다. Package·process timer 검증이 남아 있어 아직 종결하지 않는다. |
| CPP-DISP-005 | PARTIAL·검증 중 | 구현 checkpoint `388d59516b`에서 MeshNode ROUTER poller에 기존 `runtime_wake_timer_t`를 연결하고, local publish·Actor join·Actor message·Spot request enqueue가 같은 activity signal을 사용하도록 통합했다. 5초 poll 대기가 local publish 직후 500 ms 검증 한도 안에 반환되는 owner-layer 회귀를 추가했다. 전체 `test_cpp_framework_m6b_runtime`과 `test_cpp_framework_target_contract`가 통과했다. 실제 host loop의 local request latency 계측과 package process 검증이 남아 있어 아직 종결하지 않는다. |
| CPP-DISP-006 | PARTIAL·검증 중 | 구현 checkpoint `e401806c6c`에서 세 per-Spot post 경로가 close/idle-eviction admission flag를 확인한 뒤 `callback_mutex`를 유지한 상태로 serial queue enqueue까지 끝내도록 바꿨다. 따라서 sealing은 확인과 enqueue 사이에 들어오지 못하며 handler 실행은 기존처럼 lock 밖에서 진행된다. 전체 `test_cpp_framework_execution`과 `test_cpp_framework_target_contract`가 통과했다. 설치 package와 close·relocation이 실제 ingress와 경합하는 process 검증이 남아 있어 아직 종결하지 않는다. |
| CPP-TIMER-002 | GAP·검증 중 | 구현 checkpoint `80be9877f2`에서 `catch_up_bounded`가 한 dispatch 안에서 최대 설정 개수까지 연속 tick을 전달하고, 오래된 누락분은 첫 tick의 `skipped_ticks`에 기록하도록 수정했다. `max_catch_up_ticks`는 `1..INT_MAX`만 허용한다. 5개 만료를 상한 3으로 dispatch해 index 3·4·5와 skip 2를 확인하는 회귀와 전체 `test_cpp_framework_execution`은 통과했다. Package·process timer 검증이 남아 있어 아직 종결하지 않는다. |
| CPP-TIMER-003 | PARTIAL·중 | §7 타이머 자원 비비례 위반: 스케줄링은 공유 표준형이나 등록당 `zlink::timer_t`가 signaler(OS fdpair)를 즉시 생성 — 10,000 Spot × 2 타이머 = 약 40,000 fd, 등록 수에 선형인 OS 자원. 증거: `cpp/src/runtime/timers/timer_runtime.cpp:116-121`, `core/src/api/monitoring/timer_api_internal.hpp:26-47`, `core/src/runtime/core/signaler.cpp:90-93` |

만족 항목(요약): ready set 상태화, mailbox 단일 스팬(check+insert), claim serial 배타, 10 ms 시간예산 배칭, 배치 수신 3중 한도, 커넥션 rotation cursor, overrun 3정책 이름 일치 등은 충실.

### 3.8 08-object-lifecycle (객체 종류와 활성화)

| ID | 분류 | 요약 |
|---|---|---|
| CPP-LIFE-001 | PARTIAL·검증 중 | 구현 checkpoint `f27fadae2c`에서 일반 Actor·Spot message admission이 logical object ID, authority owner generation과 current owner lease를 확인하고 `ObjectGeneration`은 비교하지 않도록 수정했다. 승인된 frozen record에는 current incarnation generation을 기록한다. Message Follow와 bound-session control은 exact Actor generation을 계속 요구하며 bound-session 불일치는 `invalid_operation`으로 끝난다. Stale generation 일반 Actor request 전달·reply, current generation 정규화와 stale lease 거부 회귀를 포함한 전체 M6B, execution, cross-process mesh vertical, M6C와 target contract가 통과했다. 첫 M6B 실행은 기존 route-cache deadline timing assertion에서 한 번 실패했고 같은 binary 재실행은 통과했다. 설치 package에서 객체 재생성과 ingress가 겹치는 process E2E가 남아 있어 아직 종결하지 않는다. |
| CPP-LIFE-002 | PARTIAL·검증 중 | 구현 checkpoint `af11dabeac`에서 explicit close와 idle eviction을 같은 lifecycle operation으로 통합하고, `OnClosing` 완료 뒤 location과 local index를 해제하도록 순서를 고쳤다. Callback이 실패해도 release를 수행한 뒤 예외를 다시 전달한다. Idle-eviction 회귀가 callback 실행 중 location/context가 유지되고 완료 뒤 제거되는지 확인하며 전체 `test_cpp_framework_execution`이 통과했다. 실제 concurrent Instance activation process E2E가 남아 있어 아직 종결하지 않는다. |
| CPP-LIFE-003 | PARTIAL·상 | 공개 계약인 failure spec §4.4는 `Ready` authority의 owner lease가 만료된 상태를 Missing과 구분하고, 다른 node의 cold activation을 시작하지 않은 채 bounded `Unavailable`로 끝내도록 요구한다. Internals 06·08·10은 이를 resolver의 닫힌 결과, activation state와 liveness 책임 분리로 구현하고, 12는 same-target initial recovery root만 허용한다. 현재 resolver는 active authority를 읽을 때 owner lease availability를 별도 결과로 투영하지 않으며, request는 조회 결과가 비어 있을 때 Instance activation을 시작한다. 전송이 `NotFound`/`Unavailable`이면 route를 무효화하고 현재 call은 다시 제출하지 않지만, owner row 정리와 다음 call이 겹칠 때 Missing과 KnownUnavailable을 구분한다는 보장이 없다. 반면 startup recovery는 같은 target node RID와 lifecycle generation의 미완료 activation만 재개한다. 증거: `common/spec/31-failure-failover-policy.ko.md`, `common/spec/server/languages/cpp/interfaces/04-spots.ko.md`, `common/internals/06-routing-and-cache.ko.md`, `common/internals/08-object-lifecycle.ko.md`, `common/internals/10-liveness-and-state.ko.md`, `common/internals/12-service-wire-protocol.ko.md`, `cpp/src/runtime/locations/store_location_resolvers.hpp:390-421`, `cpp/src/runtime/channels/channel_runtime.cpp:2151-2187`, `cpp/src/runtime/stateful/public_host_runtime.cpp:1794-1918` |

만족 항목(요약): 폐쇄 kind 집합, Entry Spot 이동 경계 제외, 생성 경합 단일 factory, 생성중 비캐시, 실패 생성 정리, count+byte 이중 한도, relocation 보류 상수 등은 충실.

### 3.9 09-session-binding (세션과 Actor 바인딩)

| ID | 분류 | 요약 |
|---|---|---|
| CPP-SESS-001 | GAP·상 | §2 참조 — 원격 bind 스왑 핸드셰이크 부재 |
| CPP-SESS-002 | PARTIAL·하 | §2 "종류별 새 직렬 실행 primitive를 만들지 말라": 주 엔진(`serial_execution_queue_t`)으로 통합은 잘 되어 있으나 `service_mailbox_t`가 admission 예산·per-owner FIFO·ready set을 독립 재구현한 제2 primitive로 공존 — 스펙이 명명한 "two-domain mailboxes" 실패 패턴 구조. 증거: `cpp/src/runtime/mesh/service_mailbox.hpp:18-115` |
| CPP-SESS-003 | PARTIAL·검증 중 | 구현 checkpoint `8a709309bd`에서 공통 `serial_execution_queue_t`에 Spot·session·Actor 전달의 닫힌 lane policy 합 타입을 주입하도록 바꿨다. Spot 정책만 실행 방식과 `active`·반납 대기·이동 봉인 상태를 가지며, session 정책은 연결 열림·닫힘만, Actor 전달 정책은 별도 lifecycle 상태를 갖지 않는다. 기존 raw `allow_yield` 인자와 필드를 제거했고 yield 허용 여부는 Spot-wide 정책에서만 계산한다. Entry Spot, Spot-wide, per-Actor Spot, STREAM session과 Actor 전달 queue 생성 지점은 각각 이름이 있는 정책을 선택한다. 잘못된 lifecycle 타입 조합을 만들 수 없는지 확인하는 회귀를 포함한 전체 `test_cpp_framework_execution`과 `test_cpp_framework_target_contract`가 통과했다. `test_cpp_framework_m6c_runtime`은 변경된 fixture를 포함해 compile은 통과했지만, 실행은 이 변경과 무관한 기존 `relocation providers must be configured once before host start`에서 중단됐다. 설치 package와 process lane 검증이 남아 있어 아직 종결하지 않는다. |

만족 항목(요약): 세션 gate/Actor gate 분리, 연결 identity 쌍, 스왑 시퀀스 필터, 재연결 fresh 구축, 이동 시 연결 유지·라우트만 갱신 등은 충실.

### 3.10 10-liveness-and-state (생존 판정과 상태 공표)

| ID | 분류 | 요약 |
|---|---|---|
| CPP-OBS-001 | PARTIAL·검증 중 | 구현 checkpoint `b3094a2a13`에서 Instance-Spot activation trace context를 diagnostics mode gate 뒤에서만 만들고, `message_flow_event_t`와 문자열은 tracer의 lazy builder 안에서 생성하도록 수정했다. Request의 async terminal event는 최초 mode가 `off`가 아닐 때만 필요한 context를 한 번 snapshot한다. `test_cpp_framework_app_host`, `test_cpp_framework_message_flow`, `test_cpp_framework_target_contract`이 통과했다. Allocation 계측과 process observability E2E가 남아 있어 아직 종결하지 않는다. |
| CPP-OBS-002 | PARTIAL·검증 중 | 구현 checkpoint `68460a5d9f`에서 diagnostics level을 message entry의 ambient context에 snapshot하고, 모든 후속 tracer와 async continuation이 같은 값을 사용하도록 수정했다. `off`로 시작한 메시지는 flow ID를 할당하지 않지만 빈 flow 값과 level을 보존하므로 처리 중 level이 켜져도 일부 event만 기록하지 않는다. 반대 방향도 같은 규칙을 적용한다. `key_transitions→off`와 `off→key_transitions` 회귀를 포함한 전체 `test_cpp_framework_message_flow`, `test_cpp_framework_messaging`, `test_cpp_framework_channel_messaging`, `test_cpp_framework_execution`, `test_cpp_framework_target_contract`가 통과했다. 설치 package와 실제 장시간 handler 중 runtime level 변경 process 검증이 남아 있어 아직 종결하지 않는다. |

만족 항목(요약): 5s/15s 단일 표준, 비즈니스 메시지의 기한 비연장, 체크 신호 앱 미도달, accept-and-fail-per-call과 7-state 폐쇄 집합은 확인했다. Diagnostics public surface는 CPP-CONTRACT-DIAG-001/002 때문에 만족 항목에서 제외한다.

### 3.11 11-message-ownership (페이로드 소유권과 복사)

| ID | 분류 | 요약 |
|---|---|---|
| CPP-OWN-001 | GAP·상 | §7 수신 content-type이 **역직렬화기 선택에 전혀 사용되지 않고 검증도 없음** — 핸들러 선언 타입으로만 codec을 찾고 JSON으로 fallback. 비-JSON codec 송신자의 페이로드가 JSON 파서에 들어가 `ProtocolError`가 아닌 엉뚱한 `payloadDecodeFailed`로 실패. 스펙 §7이 기술한 "위반 구현"이 바로 C++임. 수신측 content-type→codec 캐시 자체가 부재. 증거: `cpp/include/zlink/framework/contracts/handlers/handler_registry.hpp:118`, `cpp/src/runtime/codecs/serializer.cpp:168-178` |
| CPP-OWN-002 | GAP·중 | §3 "이동 레코드를 hot path에서 만들지 말라" 위반: `raw_stateful_dispatch_t::ingest`가 **수락되는 모든 메시지**에 대해 frozen record를 canonical 인코딩해 큐에 넣고 claim 시 다시 디코딩 — 메시지당 encode→decode 왕복. 증거: `cpp/src/runtime/stateful/raw_stateful_dispatch.cpp:421-424, 569-572` |
| CPP-OWN-003 | PARTIAL·검증 중 | 구현 checkpoint `4ba8a08bae`에서 `try_claim()`이 pending map의 decoded application payload와 stateful queue의 canonical turn을 delivery에 값 복사하던 경로를 각각 이동으로 바꿨다. Claim 시 두 full buffer 사본이 추가되지는 않으며 이를 고정하는 source contract와 전체 M6B, M6C, target contract가 통과했다. Ingress에서 transport frame, decoded payload와 canonical frozen record를 동시에 보관하는 구조는 남아 있어 아직 종결하지 않는다. |
| CPP-OWN-004 | PARTIAL·검증 중 | 구현 checkpoint `f663c6250f`에서 기본 JSON serializer가 nlohmann JSON 결과를 `encoded_payload_t`에 직접 기록하고, 수신 시 그 byte span을 직접 parse하도록 바꿨다. 송신의 `message_t::from_json() → encoded_payload_t::from_raw()`과 수신의 `encoded_payload_t::to_raw() → parse_json()` 왕복을 제거했다. 정확한 JSON wire 값과 decode 회귀, source contract를 포함한 전체 serializer registry, messaging, channel messaging, execution, cross-process mesh vertical, M6B, M6C, target contract가 통과했다. 설치 package의 process allocation/copy 계측이 남아 있어 아직 종결하지 않는다. |
| CPP-OWN-005 | GAP·하 | §5 비즈니스 핸들러가 raw payload를 받는 공개 API `send_raw` 존재 — 스펙이 계약 위반으로 명시한 형태(단, `payload_view_t`는 복사 버퍼라 native storage 책임은 없음). 증거: `cpp/include/zlink/framework/contracts/handlers/handler_registry.hpp:63-82, 363-371` |
| CPP-OWN-006 | PARTIAL·검증 중 | 구현 checkpoint `2cb188e1c5`에서 이미 encoded payload를 소유한 `message_t`가 decode와 raw 변환 때 `encoded_payload_t`를 값으로 반환하던 경로를 제거했다. 내부 visitor는 payload를 `const` reference로 읽고 결과 reference를 반환하지 못하도록 compile-time으로 제한한다. Message Follow 크기 계산, fanout beacon 비교와 fanout application decode는 binding의 `bytes()` view를 사용한다. Actor·User Spot 생성 record처럼 새 소유 buffer가 필요한 경계도 `to_bytes()` 임시 vector를 거치지 않고 view에서 목적 buffer로 한 번만 복사한다. 이를 고정하는 target contract와 전체 serializer registry, messaging, cross-process mesh vertical, M6B, M6C runtime 검증이 통과했다. 설치 package를 사용하는 process copy 계측이 남아 있어 아직 종결하지 않는다. |
| CPP-OWN-007 | PARTIAL·중 | §6 큐 포화 거부 전에 envelope 디코딩+frozen 복사+canonical 인코딩 비용을 이미 지불 — 스펙의 관찰된 안티패턴. 증거: `cpp/src/runtime/stateful/raw_stateful_dispatch.cpp:279-448` |
| CPP-OWN-008 | PARTIAL·검증 중 | 구현 checkpoint `79c7f894d7`에서 custom serializer를 typed cache에 넣을 때 erased encode/decode 함수를 한 번 복사해 serializer state가 소유하도록 바꿨다. Cached serializer는 메시지마다 registry의 `std::map`을 다시 조회하지 않으며 registry object를 move한 뒤에도 기존 serializer가 동작하는 owner 회귀를 추가했다. 전체 serializer registry, messaging, channel messaging, execution, cross-process mesh vertical, M6B, M6C, target contract가 통과했다. Dynamic `encode_parts`와 `content_type(type)` 선택은 아직 메시지마다 type map을 조회하므로 이 항목은 종결하지 않는다. |
| CPP-OWN-009 | PARTIAL·검증 중 | 구현 checkpoint `985c7dadb4`에서 Spot 내부 route packet의 모든 byte field를 padding을 포함한 RFC 4648 Base64 문자열로 직렬화하도록 통일했다. 숫자 배열 팽창은 제거했고 strict decoder가 잘못된 길이·문자·padding·non-canonical pad bit를 거부한다. Known vector round-trip과 invalid input 회귀를 포함한 `test_cpp_framework_messaging`은 통과했다. Cross-language wire fixture와 실제 Spot→Actor process 검증이 남아 있어 아직 종결하지 않는다. |

만족 항목(요약): 큐 보관 중 프레임워크 소유, 핸들러 완료 후 해제, header 우선 판독·admission 전 미역직렬화(타입 역직렬화 기준), 송신측 선택 캐시(COW·lock-free) 등은 충실.

### 3.12 12-service-wire-protocol (서비스 와이어 프로토콜)

| ID | 분류 | 요약 |
|---|---|---|
| CPP-WIRE-001 | PARTIAL·검증 중 | 구현 checkpoint `abaad2368a`에서 RFC 3986 unreserved byte만 보존하고 나머지를 uppercase percent encoding하는 `zla1:<a|s>:<byte-length>:<encoded-id>` codec을 internal owner로 추가했다. In-memory·provider Store의 reserve/commit write, Actor·User Spot·Instance Spot read, resolver, relocation adapter가 모두 이 codec을 사용하며 `1:`/`2:`/`3:`과 `actor:`/`spot:` legacy Location Store fallback을 제거했다. Known key 회귀와 source gate를 포함한 전체 store resolver 34개, in-memory/provider Store, location lifecycle/runtime, app host, execution, cross-process mesh vertical, M6B, M6C, layout·target contract가 통과했다. 설치 package와 다른 언어 runtime이 같은 provider를 사용하는 process 검증이 남아 있어 아직 종결하지 않는다. |
| CPP-WIRE-002 | GAP·상 | RouteMesh ServerServer에는 Framework-level message-size 제한이 없어야 하지만 public header의 `mesh_node_socket_config_t::max_message_size`(기본 16 MiB), 이를 요구하는 Application HWM startup validation, RouteMesh descriptor/admission의 `effective_max_message_bytes`와 codec 검증이 남아 있다. Production descriptor는 설정값이 양수이면 그 값을 기록하고 `0`일 때만 4 MiB로 fallback한다. ROUTER 소켓·송신 경로에 이 값을 적용하지 않는 현재 동작 자체는 계약과 일치한다. Exact interface에서는 금지된 field와 negotiation 설명을 제거했다. 구현의 public 설정 surface·HWM 의존성과 RouteMesh wire field를 제거해야 한다. 증거: `common/spec/07-channel-topology.ko.md:609-634`, C++ exact interface `interfaces/03-channel-messaging.ko.md:81-89`, `cpp/include/zlink/framework/contracts/configuration/mesh_node.hpp:149-154`, `cpp/src/runtime/host/app.cpp:1303-1310`, `cpp/src/runtime/mesh/mesh_node_runtime.cpp:362-373`, `cpp/src/runtime/mesh/service_topology_registry.hpp:55`, `cpp/src/runtime/protocol/service_wire_codec.cpp:3769,3858,3986-3995,4039-4054` |
| CPP-WIRE-003 | GAP·상 | public codec 계약인 `framework-json-v1` 프로파일 미구현: BOM 허용(계약: 거부), 중복 속성 last-wins 허용(계약: 거부), 64-bit 정수 문자열·범위 규칙 부재, golden fixture 부재 — 다섯 언어의 동일 decode 결과를 검증할 수 없다. 증거: `common/spec/04-message-model.ko.md:95-118`, `cpp/include/zlink/framework/codecs/json.hpp:13-25` |
| CPP-WIRE-004 | GAP·검증 중 | 구현 checkpoint `985c7dadb4`에서 multicast frame, relocation state와 backlog, join snapshot, Actor packet과 bound-session payload를 모두 padding을 포함한 RFC 4648 Base64 문자열로 encode/decode하도록 수정했다. `test_cpp_framework_messaging`이 known vector `AAEC/f7/`의 정확한 wire 값과 round-trip, invalid input 거부를 확인했다. 공통 cross-language wire fixture와 C++ package를 사용하는 process E2E가 남아 있어 아직 종결하지 않는다. |
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
| logger provider 실패가 원래 operation 결과를 바꾸지 않는지 (DEC-01) | provider가 throw하는 dispatch-error process E2E를 먼저 통과한 뒤 observer·raw DTO export 제거 |
| participant state 64 MiB 경계 (DEC-17) | 64 MiB 성공과 64 MiB 초과 `StateIncompatible` process E2E |
| Ready Instance owner loss (08, CPP-LIFE-003) | owner process 종료와 lease 만료 뒤 다른 factory·handler가 실행되지 않고 모든 call이 bounded `Unavailable`로 끝나는지 검증. 미완료 initial cold activation의 same-target recovery와 별도 scenario로 실행 |
| 스키마 validator가 C++ 빌드를 실제 gate하는지 (12 §1) | 빌드 시스템 실행 확인 |
| public exact interface와 실제 설치 package 일치 | source consumer compile, install 후 clean consumer compile, `verify_packaged_contract.sh` 실행 |
| fanout 구독자가 앱 페이로드 수신으로도 기한을 연장하는 것의 적합성 (10) | Transport Liveness 스펙 오너 판정 |

---

## 5. 권고

1. **이 보고서에서 unique ID 관리** — 정식 spec에 구현 진행 기록을 다시 넣지 않고, 이 plan 문서가 30 GAP과 30 PARTIAL의 상태·완료 증거를 소유한다. 특히 CPP-TOPO-001은 종결된 `JVM-TOPO-001`, CPP-COMP-001은 종결된 `NODE-ROUTE-001`과 비교하되 다른 언어의 종결을 C++ 완료 증거로 사용하지 않는다.
2. **수정 순서 제안**:
   - 1차 (안정성·정합성): CPP-DISP-001(terminate), CPP-DISP-002(HOL), CPP-RELOC-001(영구 차단), CPP-SESS-001(이중 소유), CPP-TOPO-001(fence), CPP-ROUTE-001(dead 매처 — 한 줄급 수정), CPP-LIFE-003(Ready owner loss와 Missing 분리)
   - 2차 (public 계약·상호운용): CPP-CONTRACT-DIAG-001/002, CPP-CONTRACT-QUERY-001, CPP-CONTRACT-STREAM-001, CPP-CONTRACT-ROLE-001, CPP-CONTRACT-HTTP-001, CPP-WIRE-001(키 포맷 — 마이그레이션 계획 필요), CPP-WIRE-002(RouteMesh 금지 상한 surface·wire field 제거), CPP-WIRE-003(json-v1), CPP-OWN-001(content-type 검증), CPP-COMP-001(문자열 분류 → 구조화 오류 코드)
   - 3차 (자원·성능): CPP-TIMER-001(무한 누적), CPP-COMP-004(고아 엔트리 누수), CPP-TIMER-003(fdpair), CPP-EXEC-001/CPP-OWN-002~004(복사·락 절감), CPP-DISP-005(100 ms wake)
   - 4차 (구조·명명): 나머지 PARTIAL
3. **오류 종류 정합은 계약 테스트로 고정** — CPP-EXEC-002, CPP-FOLLOW-001과 CPP-CONTRACT-ROLE-001은 공개 오류 kind가 계약과 달라지는 계열이므로, 수정과 함께 source contract test와 cross-language process E2E에 오류 kind assertion을 추가한다. CPP-LIFE-001은 오류 이름 문제가 아니라 일반 message admission fence 자체의 의미 차이다.
4. **public surface 제거에는 package 증거가 필요하다** — source header 수정만으로 끝내지 않고 install tree에서 제거된 export를 확인하고 clean consumer를 compile한다. Diagnostics observer 제거는 provider 경로와 failure-isolation E2E가 먼저 통과해야 한다.
5. 본 리포트는 정적 코드 읽기 기반이다. §4의 항목은 판정을 유보했다. GAP 항목은 source·exact interface 차이를 확인했지만 runtime semantics를 수정할 때에는 해당 경로의 contract test와 process E2E로 재현과 종결을 각각 증명해야 한다.

## 6. 이번 재검토의 검증 결과

현재 source로 관련 C++ target을 다시 빌드한 뒤 다음 8개 CTest를 실행했고 모두 통과했다.

- `test_cpp_framework_contract_headers`
- `test_cpp_framework_client_server_runtime`
- `test_cpp_framework_channel_messaging`
- `test_cpp_framework_stream_framework`
- `test_cpp_framework_message_flow`
- `test_cpp_framework_monitoring`
- `test_cpp_framework_location_runtime`
- `test_cpp_framework_http_integration`

이 결과는 기존 test가 현재 구현을 회귀시키지 않았다는 증거다. 그러나 test가 제거 대상 diagnostics API를 여전히 허용하고 누락된 object query를 요구하지 않으므로, 남은 exact interface gap을 반증하지 않는다. 이후 checkpoint의 항목별 source test 결과는 각 행에 기록했다. 이번 재검토에서는 install package의 public export, clean consumer와 cross-language process E2E를 실행하지 않았다.
