# C++ binding R3 수정 결과

F-R3-1·F-R3-11·F-R3-12를 C++ binding에서 수정했다. 공개 API와 기존 assertion은 변경하지
않았다. Contract gate 19/19, sample smoke 7/7, 신규 regression 실행 파일 각각 5회가 모두
통과했다. Commit과 perf benchmark는 실행하지 않았다.

## 근거와 변경 범위

판정 근거는 감독자가 확인한 [R3 bindings 리뷰](spec-review/R3-bindings-summary.md)와
[D-098·D-109·D-111](decisions.ko.md)이다. 작업 branch는 `main`이며, 작업 전 C++ binding에는
기존 변경이 없었다. 다른 binding·Core·Framework·spec·site는 수정하지 않았다.

아래 원인 위치는 수정 전 코드 기준이고, 수정 위치는 최종 코드 기준이다.

## F-R3-1 — WRITABLE의 RID 재판정

- 소유 계층: Core가 submit RID echo를 보장하고, binding은 socket-local context·token으로
  waiter를 찾는다.
- Spec 조항: `core/doc/spec/core/socket/README.ko.md:989`의 part send RID echo,
  `:1074`의 REQUEST WRITABLE, `:1150`의 completion peer snapshot. D-109의 binding 투영
  문장도 같은 소유권을 지정한다.
- 원인: `bindings/cpp/src/Runtime/Messaging/completion_owner.cpp:321`의 SEND와 `:385`의
  REQUEST가 RID 길이와 byte를 다시 비교하고 불일치를 `INTERNAL_ERROR`/`EPROTO`로 바꿨다.
- 수정: 두 비교와 불필요해진 `<cstring>` include를 제거했다. Completion kind, context,
  token, send result와 terminal errno 검사는 유지했다. 재제출 대상은 기존 operation이
  보관한 submit target이다. C++의 해당 비교에는 별도 동적 할당이 없었다.
- 교차언어 대조: 현재 .NET `CompletionOwner.cs`의 SEND·REQUEST capture도 RID를 재판정하지
  않고 context·token으로 합류한다. R3 리뷰에서 지적한 같은 중복 판정의 C++ 수정이다.
- 변경 분류: **B — 기존 결함(하위 계약 재판정)**. 계약을 만족하는 Core에서는 동작이 같다.
- 수정 전/후 규칙 수: **2 → 1** — Core의 RID echo 보장과 C++ 재판정 → Core의 보장만 유지.
- Regression: `bindings/cpp/tests/contract/test_cpp_contract_writable_token_delivery.cpp:4`.
  잘못된 token은 waiter를 끝내지 않는다. 정확한 token·context의 WRITABLE에서 RID echo를
  바꿔도 SEND와 REQUEST가 각각 끝나며, 재제출 target과 payload는 원본과 같다. RID 변경은
  binding의 재판정 여부를 검사하는 native 경계 주입이며 정상 Core 동작을 재정의하지 않는다.
- Gate: 신규 regression 5/5, 전체 contract 19/19와 samples 7/7 통과.
- BLOCKERS: 없음.

## F-R3-11 — SEND 대기 토큰의 runtime 진행

- 소유 계층: binding의 socket별 completion owner.
- Spec 조항: `bindings/doc/spec/async-execution-model.ko.md` §4,
  `bindings/doc/spec/async-coroutine-policy.ko.md` §4. Public `PollCompletion` 등록이 없으면
  runtime이 owner이며, SEND와 REQUEST는 같은 completion queue를 사용한다.
- 원인: `bindings/cpp/src/Runtime/Messaging/completion_owner.cpp:593`의 SEND 등록이 runtime
  owner를 중단하고 `:630`의 drain·`:688`의 owner 시작·`:797`의 owner 복귀가
  `_send_entry_count`로 차단됐다. `async_operation_state.hpp:82`의 suspend는 continuation만
  등록하므로 owner 공백을 해결하지 못했다.
- 수정: `_send_entry_count`와 모든 SEND 전용 owner 조건을 제거했다. SEND 등록도 기존
  runtime owner를 시작한다. `completion_owner.cpp:542`에서 caller source 분리를 등록
  잠금 안으로 옮겨, 새로 진행하는 runtime이 재제출하기 전에 caller pointer를 해제한다.
  `send_operations.cpp`의 중복 detach 호출은 제거했다. Awaiter 구현은 변경하지 않았다.
- 교차언어 대조: .NET `CompletionOwner.cs:99`, `:622`와 Java
  `CompletionOwner.java:140`, `:774`는 SEND도 기존 runtime owner에서 진행한다. C++만
  operation 종류로 owner를 차단하던 분기가 원인이며, awaitable 자체의 구조적 차이는 아니다.
- 변경 분류: **B — 기존 결함(parity gap)**.
- 수정 전/후 규칙 수: **2 → 1** — REQUEST runtime/SEND public 필수 → 등록 여부로 정한 owner.
- Regression: `bindings/cpp/tests/contract/test_cpp_contract_send_runtime_owner.cpp:48`.
  TCP connect-before-bind로 실제 wait token을 만들고, 공개 `async()`와 `co_await`로
  SEND 단독, SEND+REQUEST 동시 진행, public poller 제거 뒤 runtime 복귀를 검증한다.
  세 경우 모두 public completion wait 없이 payload와 reply가 전달된다.
- Gate: 신규 regression 5/5(각 실행에 세 경우 포함), 전체 contract 19/19와 samples 7/7 통과.
- BLOCKERS: 없음.

## F-R3-12 — NO_DATA 뒤 재제출

- 소유 계층: binding의 기존 completion owner가 Core의 drain·재제출 순서를 투영한다.
- Spec 조항: `core/doc/spec/core/socket/README.ko.md:992`의 part send,
  `:1075`의 REQUEST DONTWAIT, `:1177`의 completion pull과 단일 drain owner;
  `bindings/doc/spec/async-execution-model.ko.md` §4.
- 원인: `bindings/cpp/src/Runtime/Messaging/completion_owner.cpp:360`, `:418`의 capture가
  재제출을 직접 실행했다. `:630`의 drain이 재제출로 생긴 completion까지 같은 회차에서
  읽었으며, 등록 전 SEND completion은 `:600`의 등록 thread에서도 재제출됐다.
- 수정: `completion_owner.hpp:30`의 내부 capture 결과로 pending·terminal·retry를 구분한다.
  `completion_owner.cpp:584`의 drain은 retry entry만 임시 vector에 모으고, `NO_DATA`를
  받은 뒤 기존 submit attempt를 실행한다. 재제출이 만든 completion은 다음 drain에서 읽는다.
  등록 전 SEND는 기존 early map과 등록 알림으로 같은 drain에 합류시킨다. 등록 thread의
  별도 capture·재제출 경로를 제거했다. Shutdown은 등록 합류 대기를 깨우고, receive 실패는
  이미 꺼낸 retry waiter를 오류로 끝낸다. 타이머·추가 poller·재시도 횟수 정책은 없다.
- 대안 비교: 모든 SEND를 native submit 전에 등록하면 등록 경합은 사라지지만 즉시 admission
  경로에도 owner 잠금과 등록 비용이 생긴다. 기존 submit-first와 early map을 유지하면서
  completion을 읽은 owner가 등록과 합류하는 안을 선택했다. 기존 fast-path assertion도 유지했다.
- 교차언어 대조: .NET `CompletionOwner.cs:331`의 `DrainCore`는 `NO_DATA`에서 `_retries`를
  실행한다. SEND `:1016`와 REQUEST `:1364`는 capture에서 retry를 수집한다. C++도 같은
  순서를 사용하며, submit-first 등록 경합은 C++의 기존 early map에서 처리한다.
- 변경 분류: **B — 기존 결함(spec/implementation drift)**.
- 수정 전/후 규칙 수: **3 → 1** — Core의 NO_DATA 경계와 capture·조기 등록의 직접 재제출
  → completion owner의 NO_DATA 뒤 재제출.
- Regression: `bindings/cpp/tests/contract/test_cpp_contract_completion_drain_order.cpp:4`는
  `WRITABLE 1 수신 → WRITABLE 2 수신 → NO_DATA → SEND 재제출 → REQUEST 재제출` 순서와
  새 REQUEST completion이 다음 drain까지 남는 것을 검사한다. `:32`는 SEND submit 반환 전
  WRITABLE 수신과 다음 receive를 barrier로 제어해, 등록 thread도 NO_DATA 전 재제출을
  실행하지 않는지 검사한다. 시간 지연으로 순서를 추측하지 않는다.
- Gate: 신규 regression 5/5(각 실행에 두 경우 포함), 전체 contract 19/19와 samples 7/7 통과.
- BLOCKERS: 없음.

## 원인별 diff 분리

Commit은 하지 않았다. 다음 순서로 hunks를 분리할 수 있다.

| 원인 | 구현 hunks | 테스트·빌드 hunks |
|---|---|---|
| F-R3-1 | `completion_owner.cpp`의 `<cstring>` 제거, SEND·REQUEST `routing_id_matches` 계산과 조건 제거 | `completion_native_fixture.hpp`, `test_cpp_contract_writable_token_delivery.cpp`, CMake의 해당 Linux native-wrap target |
| F-R3-11 | `completion_owner.hpp`의 `_send_entry_count` 제거, `.cpp`의 count 증감·owner 조건 제거와 SEND owner 시작·source detach 이동, `send_operations.cpp`의 detach 제거 | `test_cpp_contract_send_runtime_owner.cpp`, CMake test source 목록의 해당 행 |
| F-R3-12 | `completion_owner.hpp`의 capture 결과·retry·등록 알림 선언, `.cpp`의 capture→retry 반환·NO_DATA 뒤 실행·early SEND 합류·drain 실패 및 종료 처리, `send_operations.cpp`의 합류 주석 | `test_cpp_contract_completion_drain_order.cpp`, CMake의 해당 Linux native-wrap target; F-R3-1의 경계 fixture 재사용 |

`completion_native_fixture.hpp`의 first-submit/receive hook은 F-R3-12가 사용하는 테스트 배선이다.
F-R3-1을 먼저 분리할 때 fixture 전체를 포함해도 production 동작에는 영향이 없다.

## 검증 기록

- 수정 전 신규 테스트: **3/3 실패**. F-R3-1은 `EPROTO`, F-R3-11은 미전달로 receive assertion,
  F-R3-12는 같은 drain이 새 completion까지 읽어 processed-count assertion에서 실패했다.
  로그: `/tmp/zlink-cpp-r3-baseline.log`.
- 관련 suite: REQUEST/reply, REQUEST WRITABLE, optimization guard와 신규 테스트 **6/6 통과**.
  로그: `/tmp/zlink-cpp-r3-targeted.log`.
- 신규 테스트 반복: `ctest --test-dir bindings/cpp/build --output-on-failure --repeat until-fail:5
  -R 'test_cpp_contract_(writable_token_delivery|send_runtime_owner|completion_drain_order)$'`.
  **15/15 실행 통과**, 로그: `/tmp/zlink-cpp-r3-repeat.log`.
- 최종 gate:

  ```bash
  flock /tmp/zlink-samples-gate.lock env \
    ZLINK_CORE_SOURCE=local \
    ZLINK_CPP_CORE_BUILD_DIR=/home/hep7/project/zlink/core/build-dev \
    ZLINK_BUILD_JOBS=4 bash bindings/cpp/tests/run_tests.sh
  ```

  **Contract 19/19, samples 7/7, exit 0, `[cpp-tests] PASS`**.
  로그: `/tmp/zlink-cpp-r3-gate.log`. Lock은 script와 그 안의 sample 실행 전체에서 유지했다.
- `ldd bindings/cpp/build/test_cpp_contract_send_runtime_owner`: Core library가
  `/home/hep7/project/zlink/core/build-dev/lib/libzlink.so.0`으로 해석되는 것을 확인했다.
- `git diff --check -- bindings/cpp`: 통과. Native symbol-wrap 회귀 테스트는 Linux 전용으로
  등록하며 이번 gate도 Linux에서 실행했다. Perf benchmark는 실행하지 않았다.

## BLOCKERS

요청 범위의 남은 실패와 blocker는 없다.
