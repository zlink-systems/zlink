# C++ ZoneWorld의 중복 publisher bind 수정

## 판정과 범위

**ZoneWorld FIXED, 전체 samples gate BLOCKED**다. ZoneWorld는 3회 모두 ledger 34/34와
exit 0을 확인했다. C++ Framework unit test는 42/42 통과했다. Samples 개별 결과는
6/7이며 DeliveryDispatch route readiness 실패가 남아 있다.

rebuild11에서 드러난 원인은 C++ Framework의 discovery publish 완료 뒤 manual publisher
경로까지 실행하는 기존 결함(B)이다. Ops process 안에서 기존 PUB listener가 점유한 주소에
두 번째 XPUB listener를 bind했다. Core의 합법적인 rebind 거부나 runner의 replacement
process 중첩이 아니다. `SO_REUSEPORT`가 허용하던 중복 listener가 제거되면서 오류가 드러났다.

수정은 C++ Framework 구현과 해당 unit test에 한정한다. Core, binding, sample/runner,
shared_sample, 다른 언어와 spec은 변경하지 않는다. commit 없음.

소유 계층: Framework channel outbound의 publisher 선택과 제출 완료 경계.

spec 조항: `framework/doc/framework/common/spec/server/02-channel-transport/02-channel-messaging.ko.md:460-468`
§7의 publish Async 완료와 `05-transport-liveness.ko.md:148-176` §4의 application·beacon 동일 PUB 사용.
Core `core/doc/spec/core/socket/README.ko.md:818`은 점유 중 주소의 `EADDRINUSE`를 규정한다.
D-098 항목 1의 close/unbind 반환 후 재사용 경계는 이번 중복 bind에 도달하지 않았다.

교차언어 대조: Java·Node·.NET은 선택한 publisher에 한 번 제출한 뒤 반환한다.
C++만 discovery 제출 후 manual 제출까지 이어지는 제어 흐름 결함이 있다.

변경 분류: **B — 기존 결함**. 사용자가 지정한 “Framework 자체의 중복 bind이면 소유 모듈 수정” 범위다.

수정 전/후 규칙 수: discovery publish 한 호출이 사용하는 transport owner **2 → 1**.
기존 owner의 완료를 반환하며 새 상태·타이머·retry·포트 할당 규칙은 추가하지 않는다.

## 실패 sequence와 원인 위치

보존된 실행은
`/tmp/claude-1000/-home-hep7-project-zlink/be443aad-d720-4c23-9860-099f913fdfd4/scratchpad/cpp-zoneworld-fail-run/`이다.

1. `config/ops.json`의 broadcast endpoint는 **`tcp://127.0.0.1:20483`**이다.
   Ops mesh는 20394, Ops stream은 20330이다. 충돌 주소는 broadcast listener다.
2. `samples/ZoneWorld/Server/Ops/main.cpp:487`이 publisher를 설정한다.
   `framework/src/runtime/fanout/fanout_location_runtime.cpp:225-227`이 시작할 때 PUB을 만들고,
   `raw_fanout_owner.cpp:62`에서 bind한다. `fanout_location_runtime.cpp:268`은 그 owner를
   channel의 discovery publish callback으로 연결한다.
3. Ops의 maintenance 처리(`Server/Ops/main.cpp:397-407`)는 store 기록과 대상 적용 후
   같은 publisher로 변경 event를 발행한다. 공지 처리(`:386-390`)도 같은 경로를 사용한다.
4. `framework/src/runtime/channels/channel_outbound_exchange.cpp:1670-1675`는 discovery
   owner의 publish 완료를 기다린다. 수정 전에는 여기서 반환하지 않아 `:1705-1716`
   (수정 후 `:1706-1717`)의 manual `channel_native_publisher_t`도 생성했다.
   그 생성자의 `:882`가 **기존 PUB을 닫지 않은 상태에서 같은 endpoint에 XPUB을 bind**한다.
5. `client.log:7`의 첫 client 실패는 `A3 target maintenance was not applied`다.
   `ops.log:8`에는 `AnnounceWorldReq`의 `Unknown error 502 (errno=98)`이 남고,
   `client-d2.log:1`에도 같은 오류가 있다. E5 cleanup은 같은 maintenance publish 경로에서
   실패했다. transition client는 armed marker 전에 timeout으로 끝났다.

위 구현 경로는 모두 `framework/languages/cpp/` 기준이다. 보존 로그에는 당시 Ops의
숫자 PID나 fd trace가 없으므로 그 값은 확정하지 않는다. process 역할과 endpoint는 보존
config·로그·호출 경로로 특정했고, 동일 결함의 fd 순서는 아래 회귀 테스트에서 확인했다.

### 동일 결함의 syscall 증거

수정 전 Framework에 보강한 기존 테스트
`ZLinkFrameworkStoreLocationResolvers.AppFanoutPublishUsesLocationAutoConnect`를 실행하면
`Unknown error 502 (errno=98)`로 실패한다. 공개 Framework app·publisher 경로를 사용한다.

`/tmp/cpp-zoneworld-rebind-baseline.strace`의 순서는 다음과 같다.

| 시각 | thread | syscall | 결과 |
|---|---:|---|---|
| 05:06:59.785785 | 27468 | `bind(15, 127.0.0.1:34948)` | 성공 |
| 05:06:59.785849 | 27468 | `listen(15, 100)` | 성공 |
| 05:06:59.794363 | 27475 | `bind(26, 127.0.0.1:34948)` | `EADDRINUSE` |
| 05:06:59.833942 | 27479 | `close(15)` | 종료 시 해제 |

이들은 같은 테스트 process의 thread다. 포트 선택용 fd 3과 netlink 조회 fd의 bind/close는
listener와 구별했다. 수정 후 trace에는 port 30592의 listener bind/listen이 한 번 있으며
`EADDRINUSE`가 없다. 이전 listener 해제 뒤 새 bind가 실패하는 sequence는 관측하지 않았다.

### Runner와 replacement 대조

- C++ `samples/ZoneWorld/run_sample.sh:176-183`은 signal 뒤 `wait "$pid"`를 완료한다.
  C3 뒤 restart(`:441`), E5, G3, G4는 이 함수를 거쳐 새 process를 시작한다.
- Java `framework/languages/java/samples/java/ZoneWorld/run_sample.sh:109-114`와
  .NET `framework/languages/dotnet/samples/ZoneWorld/run_sample.sh:225-248`도 kill/TERM 뒤
  process를 기다린다. .NET E5(`:911-915`)는 stop 뒤 start한다.
- Node `framework/languages/node/samples/ZoneWorld/Runner/sample-runner.mjs:229-246`은
  stop 완료 후 replacement config를 만든다. `zoneNodeConfig`는 새 endpoint를 할당한다.
- 공통 ZoneWorld spec §9.2·§9.3과 §11.2의 고정값(`README.ko.md:706-723`)에 따라
  restart는 zone 0개인 replacement다. 따라서 `zones=`는 정상이다. 보존 로그의 초기
  `RouteMesh channel send target was not found`와 그 뒤 ready/report도 bind 충돌 증거가 아니다.

## 수정과 회귀 검증

`framework/languages/cpp/framework/src/runtime/channels/channel_outbound_exchange.cpp:1676`에
`co_return`을 추가해 discovery owner의 제출 완료를 그대로 반환한다. 대안인 runner의 stop/start
변경은 충돌 소유자를 고치지 못한다. publisher registry나 mode 분기를 추가하는 방법도 기존
owner가 이미 선택되어 있으므로 필요하지 않다.

`framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_store_location_resolvers.cpp:1405,1424,2977`은
기존 fanout test의 publish task를 `co_await`하고 관측한 오류가 없음을 단언한다. 기존 테스트는
subscriber 수신만 확인하고 버려진 task의 제출 실패를 관측하지 않아 이 결함을 통과시켰다.
기존 delivery 관측 횟수·대기·단언은 유지한다.

Core는 설치 prefix `.artifacts/wsl/install/zlink-core/0.17.0`의 rebuild11이다.
`libzlink.so.0.17.0` SHA-256은
`8c547d87217121092323179d90092e42bfa3ecdbff27e0c3b43c7ae8f0b311a4`이고,
`ldd build/sample_cpp_framework_zoneworld_ops`로 이 prefix의 library 사용을 확인했다.

| 검증 | 결과 | 보존 로그 |
|---|---|---|
| 수정 전 보강 회귀 테스트 | FAIL, `errno=98`, 중복 bind 확인 | `/tmp/cpp-zoneworld-rebind-baseline.{log,strace}` |
| `cmake --build build -j4` | PASS | `/tmp/cpp-zoneworld-rebind-build.log` |
| 수정 후 동일 회귀 테스트 | PASS, 1/1, 중복 bind 없음 | `/tmp/cpp-zoneworld-rebind-fixed.{log,strace}` |
| `ctest --test-dir build -L framework-unit --output-on-failure -j4` | PASS, 42/42, 12.52초 | `/tmp/cpp-zoneworld-rebind-unit.log` |
| ZoneWorld ×3 | PASS, 각 exit 0·ledger 34/34, 합계 102/102 | `/tmp/cpp-zoneworld-rebind-zoneworld-{1,2,3}.log` |
| `bash samples/run_samples.sh` 1회 | FAIL, exit 1; TicTacToe·Bingo 통과 후 DeliveryDispatch route readiness 실패 | `/tmp/cpp-zoneworld-rebind-samples.log` |
| 미실행 sample 개별 runner | SupportChat·GameQuest·ShoppingMall 모두 exit 0 | `/tmp/cpp-zoneworld-rebind-{SupportChat,GameQuest,ShoppingMall}.log`, `/tmp/cpp-zoneworld-rebind-remaining-samples.log` |
| 범위 내 `git diff --check` | PASS | 구현·test·보고서 |

빌드·검증 working directory는 `framework/languages/cpp`다. samples 전체 실행 묶음은
`flock -w7200 /tmp/zlink-samples-gate.lock` 아래에서 실행한다.
독립 2축 리뷰는 문서 원칙 준수와 코드 부합을 확인했으며 finding은 없었다.

## BLOCKERS

**ZoneWorld 수정은 검증 완료, samples 7/7 gate는 BLOCKED**다. 전체 runner는
DeliveryDispatch `run_sample.sh:309-310`에서
`Expected dispatch route readiness exactly 1 time(s), found 0.`으로 중단됐다.
Client 실행(`:316`) 전의 public RouteMesh readiness 확인 단계이며,
`Server/Configuration/sample_readiness.hpp:81-86`이 `snapshot.is_ready`일 때 출력하는
marker가 없었다. DeliveryDispatch의 `delivery_status_publisher_t`도 fanout이 아니라
channel request를 사용한다(`Server/Dispatch/main.cpp:178-181`). 이 실패는 수정한
discovery fanout publish 경로와 구별되며 추가 원인은 확정하지 않았다.

해당 runner는 실패 때도 `run_sample.sh:53`에서 run directory를 삭제하므로 role·flow 파일은
남지 않았다. stdout/stderr 전체는 위 samples log에 보존했다. timeout·assertion 변경이나
관련 없는 runtime 수정, 같은 전체 gate 재실행은 하지 않았다. 전체 runner가 도달하지 못한
SupportChat·GameQuest·ShoppingMall은 같은 gate lock 아래 개별 runner를 한 번씩 실행하여
모두 exit 0을 확인했다. ZoneWorld는 별도 3회에서 전체 ledger를 통과했다.
