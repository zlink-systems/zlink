# C++ DeliveryDispatch readiness 행 출력 경합 수정

## 원인과 수정 범위

**DeliveryDispatch 수정과 samples 7/7 검증은 완료했다.** 전체 unit gate는 42/43이며,
기존 M6A 테스트의 heap corruption 실패가 남았다. 해당 테스트의 단독 재실행은 통과했다.

Dispatch의 readiness marker가 여러 thread의 출력과 섞여 runner의 정확한 행 비교에서
누락되는 **sample의 기존 결함(B)**을 수정했다. 실제 sample 재현에서 RouteMesh는 ready에
도달했다. `EADDRINUSE`, 중복 listener 또는 endpoint 해제 뒤 rebind 실패를 뒷받침하는
증거는 없었다. Core 변경이 이 경합을 만들었다고 판단할 근거도 없다.

소유 계층: C++ DeliveryDispatch sample의 readiness evidence 출력. Framework runtime과 Core의 연결·수명 결정은 변경하지 않았다.

spec 조항: `framework/doc/framework/common/sample/deliverydispatch/README.ko.md:399-419` §10.1의 sample 소유 marker·정확한 문자열·수동 readiness 관찰, `:463-466`의 runner 확인과 대기 예산.

교차언어 대조: Java `DeliveryDispatchReadinessReporter.java:74-92`는 `synchronized` reporter 안에서 완성된 문자열을 `println`한다. .NET `DeliveryDispatchReadinessReporter.cs:58-97`은 한 reporting loop에서 logger에 완성된 event를 제출한다. C++은 독립 reporter thread가 `std::cout`에 문자열 조각과 개행을 따로 출력하는 구조적 차이가 있다.

변경 분류: **B — 기존 sample 결함**. Framework runtime 수정은 없으며 사용자가 지정한 sample 소유 모듈 수정 범위에 해당한다.

수정 전/후 규칙 수: readiness 출력 순서의 소유자 **독립 reporter 3개 → 공유 streambuf 1개**. 각 reporter의 기존 readiness 판단과 한 번만 보고하는 조건은 유지한다.

수정 파일은 C++ sample의 `Server/Configuration/sample_readiness.hpp`, 회귀 테스트
`tests/Zlink.Framework.UnitTests/test_cpp_framework_deliverydispatch_readiness.cpp`,
등록을 위한 `CMakeLists.txt`와 이 보고서다. Core, binding, 다른 언어, shared_sample과
보호 문서는 수정하지 않았다. commit 없음.

## 재현 sequence와 증거

C++ 경로는 `framework/languages/cpp/` 기준이다. 실행 로그는
`/tmp/cpp-deliverydispatch-readiness/`에 보존했다. 최초 DeliveryDispatch 3회는 모두 exit 0이었다.
동일 소스의 4번째 실행에서 행 출력 경합으로 exit 1이 발생했다. 추가 5~7회는 exit 0이었다.
최초 3회는 기존 보조 trace `ZLINK_DEBUG_FRAMEWORK_SPOT_DISCOVERY=1`도 사용했다.
모든 실행에서 sample의 기존 Normal message-flow file logging을 사용했다.

1. `samples/DeliveryDispatch/Server/Dispatch/main.cpp:545-552`는 route reporter 하나와
   actor-route reporter 둘을 등록한다. 각 reporter는 public runtime observation과
   snapshot worker에서 ready 상태를 받는다(`sample_readiness.hpp:35-45,114-127`).
2. 원인 위치는 수정 전 `sample_readiness.hpp:86,173-174`다. `reported.exchange(true)`는
   reporter별 중복 알림만 막는다. 서로 다른 reporter가 같은 stdout에 쓰는 행은 보호하지 않는다.
3. 실패 실행 `/tmp/tmp.bDhhbR71Rc/logs/dispatch.log:1-3`의 내용은 다음과 같다.

```text
deliverydispatch-ready kind=route node=dispatch
deliverydispatch-ready kind=actor-route node=dispatch target=courier-node-1deliverydispatch-ready kind=actor-route node=dispatch target=
courier-node-2
```

4. `run_sample.sh:250`의 `$0 == expected` 비교는 actor-route를 0건으로 센다.
   `:311-312`에서 `Expected dispatch actor route courier node 1 exactly 1 time(s), found 0.`으로
   끝났다. Client 시작 전이므로 해당 dispatch flow 파일에는 location store ready만 있으며
   application dispatch flow가 없다. 성공 실행 `/tmp/tmp.eR4ChMy3iR/`에는 marker 세 행과
   client 실행 뒤 `AssignDeliveryMsg`의 sent/received/admitted/completed가 이어진다.
5. 신규 회귀 테스트는 public `route_mesh_runtime_t` test double로 동시에 ready 알림을 전달한다.
   수정 전에는 원래 신고와 같은 route marker 누락도 재현했다
   (`regression-baseline.log`, 1.16초에 실패).

```text
deliverydispatch-ready kind=actor-route node=dispatch target=courier-node-1
deliverydispatch-ready kind=actor-route node=dispatch target=deliverydispatch-ready kind=route node=courier-node-2
dispatch
```

이 회귀 테스트는 socket이나 listener 없이 sample reporter 자체를 실행한다. 따라서 동일한
marker 누락을 설명하는 데 두 번째 binder가 필요하지 않다. 이전 aggregate의 role 로그는
삭제되어 그 실행의 정확한 출력 바이트는 확인할 수 없지만, 실제 sample의 동종 경합과
route marker 누락의 회귀 재현을 모두 확보했다. D-098 항목 1의 close/unbind 반환 후 endpoint
재사용 계약을 어기는 Core sequence는 관측하지 않아 C-API rebind repro 대상은 없다.

## 수정과 검증

`sample_readiness.hpp:87-88,175-177`에서 두 marker를 모두 `std::osyncstream(std::cout)`에
쓴다. 표준 streambuf 동기화가 완성된 행 단위 제출을 소유한다. 별도 sample mutex/helper나
reporter 상태를 추가하지 않는다. 대안인 reporter 전체 통합은 lifecycle 구조까지 변경해야 하며,
표준 stream의 기존 동기화만으로 충분하므로 선택하지 않았다. runner의 substring 비교나
대기 예산 증가는 evidence 계약을 약화하므로 적용하지 않았다.

회귀 테스트는 준비되지 않은 상태의 무출력, route·두 actor-route의 동시 출력, 중복 ready
알림에도 각 행이 정확히 한 번만 남는지를 검사한다. 경쟁을 유도하는 thread-safe capture
stream에서 32회 실행하며 실제 sample의 readiness 조건과 marker 문자열을 유지한다.

Core는 `ldd build/sample_cpp_framework_deliverydispatch_dispatch`로 설치 prefix
`.artifacts/wsl/install/zlink-core/0.17.0`의 rebuild11 library 사용을 확인했다.
`libzlink.so.0.17.0` SHA-256:
`8c547d87217121092323179d90092e42bfa3ecdbff27e0c3b43c7ae8f0b311a4`.

로그 파일명은 `/tmp/cpp-deliverydispatch-readiness/` 기준이다. 수정 후 개별 3회의 역할·flow 로그는
`/tmp/tmp.pa4EuPmFWB/`, `/tmp/tmp.lfgp1Qn7TD/`, `/tmp/tmp.ADxNZaSXH8/`에 있고,
aggregate의 DeliveryDispatch 로그는 `/tmp/tmp.8DY0hVpYZq/`에 있다.

| 검증 | 결과 | 로그 |
| --- | --- | --- |
| 수정 전 DeliveryDispatch 최초 3회 | PASS, 3/3 | `baseline-{1,2,3}.log` |
| 수정 전 추가 재현 | 4번째 FAIL, 5~7번째 PASS | `baseline-{4,5,6,7}.log` |
| 수정 전 회귀 테스트 | FAIL, route marker 행 혼합 | `regression-baseline.log` |
| `cmake --build build -j4` | PASS | `build.log` |
| 수정 후 회귀 테스트 | PASS, 32회·1.61초 | `regression-fixed.log` |
| 수정 후 DeliveryDispatch ×3 | PASS, 각 exit 0·모든 self-check | `fixed-{1,2,3}.log` |
| `bash samples/run_samples.sh` 1회 | PASS, exit 0·7/7·`sample all result=passed` | `samples.log` |
| `ctest --test-dir build -L framework-unit --output-on-failure -j4` 1회 | FAIL, 42/43·12.25초; 새 회귀 테스트 PASS | `unit.log` |
| M6A 단독 ctest 1회 | PASS, 1/1·5.12초 | `m6a-isolated.log` |
| 범위 내 `git diff --check` | PASS | sample·CMake·test·보고서 |

검증 working directory는 `framework/languages/cpp`이며 sample 실행 묶음은 모두
`flock -w7200 /tmp/zlink-samples-gate.lock` 아래에서 실행한다. runner의 cleanup 삭제 한 줄은
증거 보존을 위해 임시로 바꿨으며 원래 내용으로 되돌렸다. runner diff는 없다.

## BLOCKERS

**전체 unit gate는 BLOCKED**다. `unit.log:13-14`에서 기존
`test_cpp_framework_m6a_runtime`이 2.89초에 `Subprocess aborted`와
`double free or corruption (out)`으로 중단됐다. 나머지 42개는 통과했고 새 readiness
회귀 테스트도 1.61초에 통과했다. 원인을 분리하기 위한 단독 ctest는 5.12초에 통과했다.
전체 suite는 반복하지 않았으며 단독 성공으로 최초 전체 실패를 덮지 않는다.

M6A의 소스 `tests/Zlink.Framework.UnitTests/test_cpp_framework_m6a_runtime.cpp:3-16`과
CMake dependency 목록에는 DeliveryDispatch/sample_readiness 의존성이 없다. 변경한 헤더는
sample reporter와 신규 전용 회귀 테스트에서만 사용한다. 이번 작업에서 Framework runtime
구현은 변경하지 않았으므로 M6A heap corruption은 별도 조사 대상으로 남긴다. 정확한
할당·해제 원인은 이번 실행의 allocator 오류 한 줄만으로 특정할 수 없다.

DeliveryDispatch의 readiness 행 혼합은 해결됐고, 수정 후 개별 3회와 aggregate의
DeliveryDispatch가 모두 모든 self-check를 통과했다. 금지된 범위의 수정, assertion 완화,
재시도·대기 예산 변경은 없다.
