# C++ MeshNode local bridge permit 검증 수정

## 판정과 원인

실패 원인은 C++ Framework **테스트의 공유 queue 관측 경합**이다. Local bridge의 handler
permit 누수나 Core의 permit 계상 회귀는 확인되지 않았다.

- 수정 전 `framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_mesh_node_vertical.cpp:734`는
  두 handler가 시작하면 host 전체 `permits_in_use`가 0이라고 가정했다.
- `framework/languages/cpp/framework/src/runtime/mesh/mesh_node_host_service.cpp:2204`의 수신 pump도
  같은 application queue를 사용한다. `:2207`에서 supply를 예약하고 `:2208`에서 가져온 뒤,
  receive/dispatch 시도 후 `:2425`에서 사용하지 않은 permit을 반납한다.
- Local bridge는 같은 파일 `:2678`에서 permit을 queued로 전환하고, `:2691`에서 handler
  진입 전에 반납한다. `runtime/channels/route_handler_invoker.cpp:41`이 이 callback을 실제
  handler 호출 전에 실행한다. 이 경로는 local send를 Core REQUEST나 transport pipe에
  제출하지 않는다.
- `runtime/dispatch/application_job_queue.hpp:471`의 snapshot은 예약과 queued job을 모두
  보고한다. Handler가 반납한 직후 pump가 재예약하면 `reserved=1, queued=0, in_use=1`이
  정상이다. 기존 assertion은 이 상태를 handler permit 누수로 오인했다.

`git blame -w`와 diff로 확인한 도입 commit은 `8bae89dc0fe`(2026-08-15,
`framework: align admission and relocation ownership`)다. 이 commit이 공유 supply 예약,
handler-entry 반납과 문제의 assertion을 함께 추가했다. 오늘의 Core 변경이 Framework
permit 계상 규칙을 변경했다는 근거는 없다. 12:54 Core의 7/7 통과만으로 경합의 부재를
판정할 수 없으며, 어느 Core 변경이 경합 노출 빈도를 바꿨는지는 입증하지 않았다.

| 검토한 Core commit | 변경 경계와 이번 실패의 관계 |
|---|---|
| `40137f1bd0` | Pair 종료 시 REQUEST completion, pipe lifecycle와 receive-progress wake. Local bridge의 queued permit 소유권을 변경하지 않는다. |
| `349040d3e6` | WS/TLS connect identity, inproc command owner, duplicate ROUTER REJECT. Local bridge의 handler-entry 반납 경로와 별개다. |
| `ccb418b6ee` | 명시적 endpoint/RID 제거 시 correlation pipe로 pending REQUEST 선택. 이 local one-way send에는 해당 pending REQUEST가 없다. |
| `b63f79a3ce` | Pending inproc connect의 preamble/admission 순서. 이 테스트의 local send는 해당 연결 경로를 사용하지 않는다. |

## 수정

`framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_mesh_node_vertical.cpp:730`에서
두 handler의 gate를 닫은 채 독립 waiter가 host queue의 유일한 permit을 확보하도록 검증한다.
기존과 같은 1초 안에 확보해야 하며, 보유 중 snapshot은 `reserved=1, queued=0, in_use=1`이어야
한다. Handler가 permit을 보유하면 waiter가 진행할 수 없으므로 반납 누수를 계속 검출한다.
Promise는 callback과 공유하여 비동기 완료 중 수명을 보장한다.

두 대안을 비교했다. Global snapshot이 0이 되도록 runtime의 수신 예약을 변경하는 방법은
spec 근거가 없고 scheduling 규칙을 추가한다. 기존 FIFO permit handoff로 반납을 검증하는
방법을 선택했다. Gate, capacity=1, payload ownership, handler 순서, 예외 이후 진행과 seal
거부 검증을 유지했다. Timeout 증가, sleep, 재시도, runtime 변경은 없다.

- 소유 계층: Framework의 C++ vertical test. Runtime의 application-job permit 소유자는 기존 host queue다.
- Spec 조항: `01-execution/04-application-job-queue-and-backpressure.ko.md` §1의 `permits = reserved + queued`, §3의 callback 첫 instruction 전 반납과 source FIFO; `03-spot-actor/03-mesh-node.ko.md` §6의 local handler 준비 및 §7.3 Node direct 제출.
- 교차언어 대조: Java `ZLinkApplicationJobContext.java:69`도 첫 application instruction 전에 `handlerStarted()`로 반납한다. `ZLinkMeshApplicationDispatcherTest.java:259`의 local capacity 테스트는 수신 pump 없이 dispatcher만 구성한다. C++만 수정하는 이유는 host pump까지 구성한 테스트의 관측 범위 차이다.
- 변경 분류: **B — 기존 테스트 결함**. Framework/Core runtime 계약과 구현은 변경하지 않았다.

수정 전/후 규칙 수: 테스트의 계상 가정 **2 → 1**(spec의 예약+queued 합계와 별도의 host 전체 0 가정 → spec의 계상식만 적용). Runtime 규칙 수는 변경 없다.

## 검증 결과

검증 Core는 기존 rebuild7 package의
`.artifacts/wsl/install/zlink-core/0.17.0/lib/libzlink.so.0.17.0`이며 SHA-256은
`1c7887c36dd2f1fe133fe3a39ccbfeb9ea42d4b051b302e3ecacf160e31b116d`다.
`ldd`로 cpp unit 실행 파일이 이 package를 사용하는 것을 확인했다.

| 검증 | 결과 | 증적 |
|---|---|---|
| 수정 전 원래 vertical test | 동일 assertion 재현 | `/tmp/zlink-cpp-permits-evidence/baseline.log` |
| Local bridge만 분리한 진단 실행 | 통과 snapshot `0/0/0`과 실패 snapshot `reserved=1, queued=0, in_use=1` 확인. 기존 runtime 및 Core에 링크 | `/tmp/zlink-cpp-permits-evidence/local-probe.log:65` |
| 최종 코드 vertical `--repeat until-fail:5` | 5/5 PASS | `/tmp/zlink-cpp-permits-evidence/vertical-repeat5.log` |
| 전체 cpp unit (`-L framework-unit`) | **41/41 PASS**, exit 0, 46.23초 | `/tmp/zlink-cpp-permits-evidence/cpp-unit.log` |
| cpp sample aggregate, 7 samples와 self-checks | **7/7 PASS**, exit 0, `sample all result=passed` | `/tmp/zlink-cpp-permits-evidence/cpp-samples.log` |
| Core worktree targeted ctest 및 patch | 해당 없음: Core 결함/수정 없음 | Core worktree와 main `core/build-dev`를 변경하지 않음 |

진단용 출력과 독립 실행 파일은 `/tmp/zlink-cpp-permits-evidence/`에만 있으며 저장소 runtime에
임시 logging을 추가하지 않았다. 보호된 문서, binding과 다른 언어는 수정하지 않았고 commit은
하지 않았다. 요청된 `framework/languages/cpp/AGENTS.md`는 현재 checkout에 없다.

Aggregate는 `flock -w7200 /tmp/zlink-samples-gate.lock` 안에서 한 번 실행했다.
Lock 획득 시각은 2026-09-05 07:57:51 UTC이며, 실행 시작과 종료 시 Core hash가 위 rebuild7
hash와 같았다. TicTacToe, Bingo, DeliveryDispatch, SupportChat, GameQuest, ShoppingMall,
ZoneWorld의 완료 표식과 aggregate 최종 성공 표식을 확인했다. ZoneWorld의 ZW-B8, ZW-G4 및
scenario ledger도 PASS다. GameQuest의 `Killed` 출력은 runner `:343`의 의도된 `kill -9`와
`:348`의 exit 137 검사로 수행하는 owner-loss self-check다.

검증 명령:

```bash
cd framework/languages/cpp
ctest --test-dir build -R zlink_cpp_framework_mesh_node_vertical_test --output-on-failure --repeat until-fail:5
ctest --test-dir build -L framework-unit --output-on-failure
flock -w7200 /tmp/zlink-samples-gate.lock bash samples/run_samples.sh
```

## BLOCKERS

없음. 남은 테스트 실패 없음. Core 수정 조건에 해당하지 않으므로 detached worktree와
`core-cpp-permits.patch`는 생성하거나 변경하지 않았다.
