# Python binding R3 수정 결과

F-R3-1과 F-R3-13의 Python 구현을 수정했다. 새 회귀 테스트 12개는 native 확장과 Python
fallback에서 모두 통과하며, 5회 반복도 모두 통과했다. 전체 gate는 기존 RID 재검증 테스트
1건 때문에 실패한다. 해당 검사의 교체 승인을 요청했으며 기존 assertion은 수정하지 않았다.

작업 branch는 `main`이다. 커밋하지 않았다. 변경 범위는 아래 Python 파일과 이 보고서뿐이다.
기존 다른 binding·Framework 변경은 수정하지 않았다. `bindings/AGENTS.md`와
`bindings/python/AGENTS.md`는 없으며 루트 규칙과 `doc/AGENTS.md`를 적용했다.

## F-R3-1 — WRITABLE RID 재검증 제거

- 소유 계층: Core가 completion의 submit RID echo를 보장하고, Python completion owner는
  socket-local token/context로 찾은 waiter에 결과를 전달한다.
- Spec 조항: `core/doc/spec/core/socket/README.ko.md:990`의 part send WRITABLE record,
  `:1073`의 REQUEST WRITABLE, `:1149`의 completion pull과 ownership, `:1181`의
  ID/context 구분. `bindings/doc/spec/async-execution-model.ko.md` §4·§5와
  `async-coroutine-policy.ko.md` §3·§4의 단일 owner·완료 합류를 유지한다.
- 교차언어 대조: `bindings/c/include/zlink/socket/api.h:225`의 C raw binding은 Core의
  token/context WRITABLE을 그대로 노출하고 추가 RID 판정을 하지 않는다. R3 진단의
  일곱 고수준 언어 중 Python만 이 작업 범위이며 다른 binding은 수정하지 않았다.
- 변경 분류: **A — 계약 적응**. Core 계약을 만족하는 completion의 결과와 재제출 target은 같다.
- 원인 위치(수정 전): `bindings/python/src/zlink/_runtime/messaging/routed_async.py:815`
  `_completion_target()`가 비교용 bytes를 만들고, `:845` `_capture_writable()`가
  `target != entry.target`을 INTERNAL_ERROR/EPROTO로 바꿨다.
  `bindings/python/src/zlink/_native/hotpath.h:1071`의 C drain도 같은 Python 함수를 호출한다.
- 수정: 비교용 helper와 RID 읽기·비교를 제거했다. 현재 `_capture_writable()`는
  `routed_async.py:838`에 있다. token·context·kind, send result와 terminal errno 검사는 유지했다.
  C drain은 기존 공통 capture 함수를 계속 호출하므로 RID 판정 규칙을 따로 추가하지 않았다.
- 대안 비교: RID 비교를 native 코드로 옮기면 재판정 규칙이 남는다. Core의 보장을 그대로
  사용하는 대안을 선택해 비교와 비교용 할당을 함께 제거했다.
- 수정 전/후 규칙 수: Python 범위 **2 → 1**(Core echo 보장 + binding 재검증 → Core 보장).
  캠페인 전체의 8 → 1 달성을 이 Python 작업만으로 주장하지 않는다.
- 회귀 테스트: `bindings/python/tests/test_completion_projection_contract.py:43`
  `test_writable_delivers_to_registered_waiter_without_reading_rid_echo`.
  native/Python × SEND/REQUEST × token/context 조회의 8개 조합이다. 정상 submit RID를 담은
  record를 주입한 뒤 binding의 RID 필드 읽기를 차단하여 재검증·비교용 생성을 검출한다.
  해당 waiter만 전달받고, 원래 target이 유지되며, 다른 token은 대기 상태를 유지하고,
  native completion이 정확히 한 번 닫히는 것을 확인한다. 잘못된 Core RID를 정상 계약으로
  취급하는 테스트는 추가하지 않았다.
- 검증 결과: 수정 전 8개 모두 RID 읽기에서 실패, 수정 후 8개 모두 통과. 반복 5회 모두 통과.
- BLOCKERS: 아래 전체 gate의 기존 `[rid]` 검사 1건과 교체 승인 대기.

## F-R3-13 — 토큰 없는 BACKPRESSURED 전달

- 소유 계층: Core가 submit result·errno·completion ID 조합을 결정하고 Python은 기존
  `SubmitError`로 전달한다.
- Spec 조항: `core/doc/spec/core/socket/README.ko.md:977`의 unified completion reservation,
  `:1060`의 REQUEST slot 포화 BACKPRESSURED/EAGAIN/ID 0, `:965`의 blocking SEND
  SNDTIMEO 만료. `bindings/doc/spec/async-execution-model.ko.md` §5 step 1과
  `async-coroutine-policy.ko.md` §3은 토큰 없는 실패의 state 제거와 exact submit error를 정한다.
- 교차언어 대조: C++ `bindings/cpp/src/Runtime/Messaging/operation_submit.hpp:225`,
  Go `bindings/go/internal/native/dealer_router_request.go:369`,
  Node `bindings/node/src/zlink/runtime/messaging/completion_owner.ts:343`은
  BACKPRESSURED/EAGAIN/nonzero ID만 대기로 연결하고 tokenless 실패의 원래 오류를 보존한다.
  Python의 이탈은 언어의 구조적 요구가 아닌 기존 오류 분류 결함이다.
- 변경 분류: **B — 기존 결함**. 합법적인 Core BACKPRESSURED가 INTERNAL_ERROR로 바뀌던 문제다.
- 원인 위치(수정 전): `bindings/python/src/zlink/_native/hotpath.h:1487`의
  `py_start_request`, `bindings/python/src/zlink/_runtime/messaging/routed_async.py:1116`의
  `_attempt_request`, `:1207`의 `submit_request`가 ID 0을 EPROTO로 재분류했다.
- 수정: BACKPRESSURED/EAGAIN/nonzero ID만 기존 WRITABLE 대기 분기로 들어간다.
  ID 0은 기존 실패 분기를 사용하여 BACKPRESSURED/EAGAIN을 그대로 전달하고 registry·payload를
  정리한다. 최초 native 제출, Python fallback 제출, 공통 Python 재제출을 맞췄다.
  현재 위치는 `hotpath.h:1487`, `routed_async.py:1104`, `routed_async.py:1196`이다.
- 대안 비교: ID 0 전용 오류 분기를 추가할 수도 있으나, 대기 조건을 좁혀 기존 실패 처리를
  재사용하는 대안을 선택했다. 새 상태·helper·poller·retry·timeout을 추가하지 않았다.
- 수정 전/후 규칙 수: **2 → 1**(Core submit 분류 + binding INTERNAL_ERROR 재분류 → Core 분류).
- 회귀 테스트: `bindings/python/tests/test_completion_projection_contract.py:114`
  `test_request_tokenless_backpressure_preserves_core_error`.
  native/Python × 최초 제출/WRITABLE 후 재제출의 4개 조합이다. 공개
  `dealer.request().message(...).timeout(1).submit()` 호출에서 Core submit 경계에
  BACKPRESSURED/EAGAIN/ID 0을 주입하고 result·errno, 추가 대기 없음,
  retained payload와 registry 정리를 확인한다. 실제 65,536개 slot 포화 대신 반환 조합을
  결정적으로 주입했다. 기존 blocking SEND·REQUEST 오류 전달과 public signature는 수정하지 않았다.
- 검증 결과: 수정 전 4개 모두 INTERNAL_ERROR/EPROTO 오분류로 실패, 수정 후 4개 모두 통과.
  반복 5회 모두 통과했다.
- BLOCKERS: 이 원인 자체의 잔여 실패는 없다. 전체 gate는 F-R3-1의 기존 검사에 막혀 있다.

## 원인별 diff 분리

두 원인의 runtime hunk는 겹치지 않는다. 다음 순서로 분리할 수 있으며 커밋은 수행하지 않았다.

| 원인 | 파일·hunk |
|---|---|
| F-R3-1 | `routed_async.py`의 `_completion_target` 삭제와 `_capture_writable` RID 읽기·비교 삭제, 즉 현재 diff의 앞 3개 hunk. 새 `test_completion_projection_contract.py`의 공통 import·`completion_runtime` fixture와 첫 번째 테스트. |
| F-R3-13 | `routed_async.py`의 `_attempt_request`·`submit_request`, 즉 뒤 2개 hunk. `hotpath.h`의 `py_start_request` hunk 전체. 새 테스트 파일의 `asyncio`·`errno` import와 두 번째 테스트. 공통 runtime 선택 fixture를 앞 diff에서 재사용한다. |

`hotpath.h:1071`은 F-R3-1의 기존 호출 경로 확인 지점이며 변경 hunk가 아니다. 강제 재빌드한
확장에서 해당 호출 경로를 실제로 검증했다. 보고서는 두 원인의 공통 검증 기록이다.

## 빌드·gate 결과

Core와 확장 모두 아래 경로를 사용했다. `/proc/self/maps`에서도 Core 하나가
`core/build-dev/lib/libzlink.so.0.17.0`으로 로드된 것을 확인했다.

- Python: `/tmp/zlink-python-r3-venv/bin/python`(3.12, 시스템 Python에 pytest가 없어 만든 임시 venv).
- Core SHA-256: `64567f1715b3f1527afbc1c290e2b262d02d722768e160227a6f9815bdd4bb43`.
- 확장: `bindings/python/src/zlink/_native/_zlink_native.cpython-312-x86_64-linux-gnu.so`.
  SHA-256: `5f33f97c9fb255fdcec81baf2606ad7662592c1194fa591f106278bc030735ea`.
- 빌드: `bindings/python`에서 `ZLINK_CORE_SOURCE=local`,
  `ZLINK_CORE_INCLUDE_DIR=/home/hep7/project/zlink/core/include`,
  `ZLINK_CORE_LIB_DIR=/home/hep7/project/zlink/core/build-dev/lib`를 지정하고
  `python setup.py build_ext --inplace --force` 실행, 성공.
  로그: `/tmp/zlink-python-r3-build.log`.

| 검증 | 결과 | 로그 |
|---|---|---|
| 새 테스트, 수정 전 | 예상한 결함 12개 재현 | `/tmp/zlink-python-r3-regression-before.log` |
| 새 테스트, 수정 후 | 12 passed | `/tmp/zlink-python-r3-regression-after.log` |
| 새 테스트 5회 반복 | 매회 12 passed, 총 60/60 | `/tmp/zlink-python-r3-regression-{1,2,3,4,5}.log` |
| 기존 completion/request writable 관련 테스트 | 41 passed, 기존 RID 검사 1 failed | `/tmp/zlink-python-r3-related.log` |
| 전체 `tests/run_tests.sh` | **227 passed, 1 failed, 4 subtests passed** | `/tmp/zlink-python-r3-gate.log` |
| 샘플 | **7/7 passed** | `/tmp/zlink-python-r3-samples.log` |

전체 gate 명령:

```bash
flock /tmp/zlink-samples-gate.lock env \
  ZLINK_CORE_SOURCE=local \
  ZLINK_LIBRARY_PATH=/home/hep7/project/zlink/core/build-dev/lib/libzlink.so \
  LD_LIBRARY_PATH=/home/hep7/project/zlink/core/build-dev/lib \
  PYTHON_EXECUTABLE=/tmp/zlink-python-r3-venv/bin/python \
  bash bindings/python/tests/run_tests.sh
```

`core/build`가 없어 명시한 `ZLINK_LIBRARY_PATH`가 유지된다. Gate가 pytest 실패로 samples 호출
전에 종료하므로, 같은 환경과 `/tmp/zlink-samples-gate.lock`의 `flock` 아래에서 gate가 호출하는
`bindings/python/samples/run_samples.sh`를 별도로 실행했다. 성능 benchmark는 실행하지 않았다.
전체 gate에 포함된 perf runner의 단위 검사는 실행했다.

## BLOCKERS

`bindings/python/tests/test_completion_contract.py:288`의
`test_writable_completion_rejects_mismatched_send_correlation[rid]`는 RID가 다르면
`INTERNAL_ERROR`로 settle해야 한다고 요구한다. 실패 assertion은 `:318`의
`assert entry.settled`다. 이 기대는 이번 작업의 명시된 F-R3-1 계약과 반대이며, 수정 전
재검증 정책을 고정한다. 다른 실패는 없다.

작업 지시의 “Never lower an assertion”에 따라 기존 검사는 변경하지 않았고 교체 승인을
질의했다. 제안은 param의 `rid`를 `kind`로 바꾸고 해당 입력을 REQUEST kind로 설정하는 것이다.
기존 token/context 검사와 모든 오류 assertion을 그대로 유지하고, RID 읽기 금지·정상 token
전달은 새 8개 회귀 검사가 담당한다. 승인 후 이 fixture 변경을 F-R3-1 diff에 포함하고 전체
gate를 다시 실행해야 0 failures 완료 조건을 충족한다.
