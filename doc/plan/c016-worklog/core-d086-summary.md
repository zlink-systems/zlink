# D-086 Core 조사·수정 결과

## 결론

TCP HANDOVER가 이전 pipe 종료를 기다린 것이 아니다. 새 TCP connection이 같은 RID의 기존
accepted transport pair ID를 재사용했고, count-1 Application lane 두 개가 같은 pair-table
slot에 들어가면서 기존 pipe와 새 pipe가 모두 거부·종료됐다. 두 DEALER가 다시 연결할 때마다
같은 충돌이 반복되어 admission 시점이 reconnect 경쟁에 따라 달라졌다.

count-1 connection마다 새 accepted pair ID를 배정하도록 고쳤다. count-2 transport는
Application/Completion 두 lane이 하나의 pair로 모여야 하므로 기존 RID 매핑을 계속 재사용한다.

## 원인 (`file:line`)

1. Network passive READY는
   `core/src/runtime/engine/asio/asio_zmp_engine.cpp:644`에서 peer RID를 기준으로
   `adopt_accepted_transport_pair()`를 호출한다.
2. 수정 전 `core/src/runtime/sockets/common/socket_base_api.cpp:66`의 함수는 lane count를 받지
   않았고, 같은 RID가 `_accepted_transport_pairs`에 있으면 기존 pair ID/generation을 그대로
   반환했다. 일시 계측에서 새 TCP connection이 기존 `pair=.../1`을 재사용하는 것을 확인했다.
3. 그 결과 새 count-1 Application pipe가 기존 pipe와 같은 pair key로
   `core/src/runtime/sockets/common/socket_base_api.cpp:317`에 들어갔다.
   `core/src/runtime/sockets/common/socket_base_api.cpp:328`의 중복 lane 검사에서 incoming/기존
   pipe를 모두 reject 대상으로 만들었다. 일시 계측은 같은 pair ID에서 서로 다른 두 pipe가
   이 분기를 밟는 것을 확인했다.
4. 충돌은 ROUTER RID 정책을 적용하는
   `core/src/runtime/sockets/router/router_admission.cpp:337`보다 먼저 발생했다. 따라서
   `router_admission.cpp:398-427`의 기존 route standby 전환과 새 route publish는 실행되지
   않았고, TCP session 종료와 재연결만 반복됐다. reconnect interval 10 ms에서 더 악화된
   이유도 이 충돌 빈도가 증가했기 때문이다.
5. inproc은 ZMP passive READY의 accepted-pair RID 매핑을 통과하지 않는다.
   `core/src/runtime/core/ctx_inproc_registry.cpp:232-261`에서 connect pipe가 가진 connection별
   pair ID/generation을 직접 사용하므로 같은 RID라도 pair-table slot이 충돌하지 않았다.

Baseline 확인:

- 고유한 `A`/`B` marker로 replacement의 payload만 admission으로 판정했다.
- 수정 전 tcp/reconnect 10 ms는 3/3회 모두 6000 ms 안에 replacement marker가 오지 않았다.
- 같은 바이너리의 inproc/reconnect 10 ms는 3/3회 0 ms였다.
- 일시 계측을 켠 tcp/reconnect 1000 ms 한 회에서는 pair 충돌 뒤 재연결 세대가 우연히 이겨
  115 ms에 들어왔다. 즉 지연은 이전 pipe 종료 완료 조건이 아니라 pair 충돌 후 reconnect
  경쟁의 결과였다.

## Spec 대조

- `core/doc/spec/core/socket/README.ko.md:151`, `:159-165`: HANDOVER에서는 같은 방향의 새 pipe가
  기존 pipe를 인수한다.
- `core/doc/spec/core/socket/07-router.ko.md:153`: 기존 active duplicate는 standby에서 상태를
  보관하고 나중에 다시 선택될 수 있다.

Spec은 새 pipe admission 전에 기존 pipe의 종료 확인을 기다리라고 요구하지 않는다. 오히려
기존 pipe를 standby로 유지하는 계약이므로, pair-table 단계에서 양쪽을 종료한 현재 동작은
구현 결함이다. Spec 변경이나 별도 상한 결정은 필요하지 않다.

## 수정

- `core/src/runtime/sockets/common/socket_base_api.cpp:66-136`
  - accepted pair 배정에 negotiated lane count를 전달한다.
  - count-1은 같은 RID 매핑이 있어도 새 pair ID를 만들고 기존 매핑을 교체한다.
  - 이전 pipe의 release는 `core/src/runtime/sockets/common/socket_base_api.cpp:139-158`에서 pair
    ID/generation이 일치할 때만 매핑을 지우므로 새 owner를 지우지 않는다.
  - count-2는 기존 매핑을 재사용해 Application/Completion lane 결합을 유지한다.
- `core/src/runtime/engine/asio/asio_zmp_engine.cpp:644-646`: READY에서 확정한 lane count를 accepted
  pair 배정에 전달한다.
- `core/src/runtime/sockets/common/socket_base.hpp:559`: 내부 함수 시그니처를 맞췄다. 공개 C API와
  ABI 변경은 없다.
- 대안으로 pair table이 같은 pair key에 여러 Application lane을 보관한 뒤 ROUTER가 고르게 할
  수 있으나, pair key당 lane 하나라는 transport-pair 불변식과 count-2 검증 범위를 넓게 바꾼다.
  충돌의 소유자인 accepted pair ID 배정에서 count-1 physical connection을 구분하는 방식을
  선택했다.

## 회귀 테스트와 분포

`core/tests/integration/test_ctx_term_fixed_rid_handover.cpp`를 추가하고
`core/tests/CMakeLists.txt:88`, `:361-365`에 등록했다.

- 공개 C API만 사용한다.
- labels: `integration;serial`, `TIMEOUT 30`.
- tcp/inproc × 이전 DEALER reconnect interval 10/100/1000 ms × 각 20회.
- prior와 replacement의 payload marker를 다르게 해 이전 pipe backlog를 replacement admission으로
  잘못 세지 않는다.
- 각 반복 뒤 replacement, prior, ROUTER를 linger 0으로 닫고 `zlink_ctx_term()` 성공도 확인한다.
- TCP p95가 200 ms 이하인지 검사한다.

Release+LTO (`core/build-d086`) 결과, 단위 ms:

| transport | prior reconnect (ms) | n | min | p50 | p95 | max |
|---|---:|---:|---:|---:|---:|---:|
| tcp | 10 | 20 | 5 | 5 | 6 | 6 |
| tcp | 100 | 20 | 5 | 5 | 6 | 6 |
| tcp | 1000 | 20 | 5 | 5 | 6 | 7 |
| inproc | 10 | 20 | 0 | 0 | 0 | 0 |
| inproc | 100 | 20 | 0 | 0 | 0 | 0 |
| inproc | 1000 | 20 | 0 | 0 | 0 | 0 |

RelWithDebInfo/no-LTO에서도 TCP 세 셀은 모두 `min 4 / p50 5 / p95 6 / max 6 ms`, inproc은
모두 0 ms였다. 따라서 Debug overhead에 따른 현상이 아니다.

## Gate

- 격리 빌드: `core/build-d086`, Release+LTO, tests ON, `-j3`: 성공.
- 전체 `ctest --test-dir core/build-d086 --output-on-failure -j2`: **143/143 성공**,
  198.39 s. integration 92, regression 25, unittest 28, wake-invariant 4, hotpath 1을 포함한다.
- 신규 테스트 Release 반복: **5/5 성공**, 회당 0.82-0.85 s.
- 관련 `test_ctx_term_fixed_rid_handover`, `test_connect_rid`, `test_router_handover`: **3/3 성공**.
- `^test_single_lane_`: **29/29 × 2 성공**, 19.26 s / 19.31 s.
- `hotpath_gate` Release+LTO:
  - dealer_dealer_inproc 3438.949/reference 3455.381 = 0.9952 PASS
  - dealer_router_reqrep_inproc 12009.444/reference 12054.895 = 0.9962 PASS
  - pair_inproc 2683.213/reference 2681.957 = 1.0005 PASS
  - router_router_tcp 2969.196/reference 2972.882 = 0.9988 PASS
- `git diff --check`: 성공.
- 임시 `ZLINK_D086_TRACE` 계측은 제거했으며 남은 일시 로그 코드는 없다.

수정 위치는 socket handshake/pair admission cold path다. send/recv hot path는 수정하지 않았다.
그래도 Release+LTO `hotpath_gate`를 실행해 위 수치로 통과했다.

참고: 요청된 dev 동등 구성(RelWithDebInfo/no-LTO)의 첫 전체 ctest에서는 기능 테스트 142개가
모두 성공했지만, Release/LTO reference를 O2/no-LTO binary와 비교한 `hotpath_gate`만 네 셀
1.25-1.32배로 실패했다. 같은 build directory를 CONTRIBUTING의 Release+LTO gate 구성으로
재구성한 최종 결과는 위와 같이 전체 green이다.

## BLOCKERS

없음. Spec 수정, 공개 API 변경, 보호 문서 변경은 필요하지 않다.

## 머신 A 후속 필요 여부

Core 쪽 수정 후 머신 A에서 local Core library/binding package를 갱신하고 기존 .NET framework
handover 테스트를 다시 실행하는 검증은 필요하다. 2 s 기대치를 늘리는 workaround나 Framework
handover 로직 변경은 D-086 해결에 필요하지 않다. admission 실패 시 replacement를 확실히 닫는
Framework 방어 코드는 별도 hardening으로는 유효하지만 이번 Core 원인의 필수 후속은 아니다.
