# m1672d — #1672 C++ 수신 역할 인계

설계 A를 구현했다. Request-serial의 I4+E5는 2.0000 → **0.0038/message**로 목표 ≤0.1을 만족한다. Send-saturation I4는 **0.6286/message**로 시험 6의 ≤0.1 예상에 미달한다. 이 drain 정책은 바꾸지 않았으며 감독자 결정이 필요하다.

작업 위치 `/root/worktree/zlink-1672-cpp-2`, 브랜치 `framework-cpp/1672-per-message-cost-2`. 시작 `f96709ec90`. 설계 근거는 `/root/jobs/fx/cx/d1672-handoff.md` §2 A·§5다. Push·PR·rebase·main 추가 merge는 하지 않았다. Working tree는 깨끗하다.

## Commit

| 단계 | commit | 내용 |
|---|---|---|
| 1 | `e11aa0c13d` | executor 역할 목록·I4 인계·application drain→역할 stop→join |
| 2 | `2003ddd563` | 회전 범위 E5 수집·첫 완료 하나 실행·완료 전용 회전과 종료 회귀 시험 |
| 3 | `d9b32811cf` | 횟수 결과를 빈 commit 본문에 기록; production 계측 코드는 남기지 않음 |

세 commit 본문은 모두 `Refs #1672`로 끝난다. 계측한 최종 runtime 코드는 2단계 commit과 같다.

## 소유 계층과 계약

| 결정 | 소유 계층·spec | 변경 전 | 변경 후 |
|---|---|---|---|
| 수신 역할의 idle·독점 배정 | Framework executor, `08-messaging-hot-path.ko.md:275` §4.4 | host의 node별 `_threads`와 executor application FIFO | `_application_dispatch`가 기존 `_mutex` 아래 역할 목록을 먼저 배정 |
| 수신·관리·permit·owner queue | Framework mailbox·gate, hot path §4.1(:141)·§4.2(:193), `02-handler-turn-and-execution-gate.ko.md` §1–3·§13(:465) | 기존 수신 loop·mailbox·gate | 같은 queue·gate·permit 순서, 역할 객체가 poller·supply·management 수명을 보유 |
| terminal take·완료 slot·callback turn | Framework registry, hot path §3(:66) E5, `01-submit-and-completion.ko.md:389` §11 | registry take→공유 dispatcher | take 위치 유지; 회전 동안 기존 item을 수집하고 경계에서 배치 |
| socket·poller·binding completion drain | Core·binding | raw owner·raw_route_port의 기존 객체·mutex | 같은 객체를 직렬 사용; 재생성·두 번째 poller 없음 |

성공 시 옛 holder는 첫 완료 하나 또는 첫 ready owner turn 하나만 실행한다. 나머지 완료·owner는 그 전에 `post_chain`·`try_submit_internal`로 보낸다. 인계 실패·stopping에서는 기존 경로를 쓴다. 회전 예외는 terminate를 유지한다. ClientServer는 범위 밖이다.

교차언어 대조: Java `runtime/internal/service/ZLinkServiceCompletionDispatcher.java:19–29,47–104`의 공유 worker, .NET `Runtime/Service/ZLinkManagedMeshNode.cs:436–443,5364`의 전용 receive loop·RCA, Node `runtime/foundation/raw-service-mesh-runtime.ts:655,679`의 event loop와 대조했다. `q1645r-report.md` §2.2도 확인했다. Hot path §4.4·§6이 이 실행 자원 차이를 허용하며 public 결과·gate·terminal 계약은 유지한다.

변경 분류: **A — 이미 정한 §4.4 계약 적용.** 스펙 gap·상위 계층 보상·새 retry는 없다. 보호된 문서는 수정하지 않았다.

수정 전/후 규칙 수: 이 경로의 Framework thread 소유자 **3→2**, 수신 경계의 배치 판정 위치 **2→1**. 역할 목록·`_role_active`·회전 범위 TLS 수집 포인터를 추가하고 `_threads`·node별 thread lambda를 제거했다. 새 lock·cache·timer는 없다.

## 단계별 횟수

각 실행은 warmup 2,000건 뒤 N=10,000건이다. Typed JSON payload body는 1,024 bytes, window·send 폭은 100이다. 시간은 측정하지 않았다.

- I4: ingress claim thread → handler 첫 명령. E5: 완료 수집 thread → request continuation 첫 명령. 즉시 수락된 send는 E5 전환 0으로 센다.
- Alloc·mutex는 trace를 넣지 않은 별도 binary의 프로세스 전체 호출 수/message다(Core·binding·Framework·동일 public harness 포함). Thread id·인계·독점 검사는 별도 trace binary에서 센다.
- S/C는 server/client다. 인계 성공·실패 열은 측정 구간의 원시 횟수이며 전환 수에 합산하지 않는다. Rotation batching과 arm 경계 때문에 인계 수가 N과 같을 필요는 없다.
- Base는 시작 commit `f96709ec90`, step1은 `e11aa0c13d`, head는 `2003ddd563`다. 세 단계는 동일한 전용 일반 Core·binding package를 사용했다.

| 시나리오 | 단계 | I4/message | E5/message | alloc S/C | mutex S/C | 인계 성공 S/C | 실패 S/C |
|---|---|---:|---:|---:|---:|---:|---:|
| request-serial | base | 1.0000 | 1.0000 | 176.2649/211.0349 | 167.4649/214.3649 | 0/0 | 0/0 |
| request-serial | step1 | 0.0000 | 1.0000 | 170.6766/210.4200 | 166.0865/213.6055 | 10000/0 | 0/0 |
| request-serial | head | 0.0001 | 0.0037 | 173.7855/210.8188 | 166.3807/217.4629 | 9999/9967 | 0/32 |
| send-saturation | base | 1.0000 | 0.0540 | 85.5046/66.4584 | 48.5087/44.3338 | 0/0 | 0/0 |
| send-saturation | step1 | 0.8263 | 0.0126 | 79.7541/65.4201 | 33.6257/41.9429 | 3/0 | 0/0 |
| send-saturation | head | 0.6286 | 0.0474 | 77.4767/64.7836 | 27.9092/40.2125 | 9/6 | 0/0 |
| request-window | base | 1.0000 | 1.0000 | 129.4102/188.4446 | 70.3138/144.1296 | 0/0 | 0/0 |
| request-window | step1 | 0.4451 | 1.0000 | 131.3570/189.6451 | 76.4493/148.9981 | 415/0 | 0/0 |
| request-window | head | 0.3452 | 0.9029 | 135.5511/193.7986 | 87.1780/160.7721 | 306/971 | 0/101 |

Request-serial 합계는 (I4 1 + E5 37)/10,000 = **0.0038/message**다. Alloc S/C는 176.2649/211.0349 → 173.7855/210.8188, mutex S/C는 167.4649/214.3649 → 166.3807/217.4629다. 전환 제거가 client mutex 호출 감소를 보장하지는 않는다.

## 시험 1–9

| 번호 | 결과·근거 |
|---|---|
| 1 application을 붙잡은 동안 진행 | 통과. 실제 RouteMesh에서 application worker 16개를 latch로 붙잡은 상태로 unrelated request 완료·liveness ack·management 진행 확인. `head-progress.log`의 `ALL_HELD → COMPLETION_PROGRESS → PROGRESS_OK`. |
| 2 역할 holder의 application 진입 금지 | 통과. 실제 세 시나리오와 held-worker 시험에서 role TLS flag를 callback·dispatch·owner drain 진입에서 검사. |
| 3 socket별 receiver ≤1 | 통과. 역할별 회전 CAS와 인계 경계 `poller_depth==0` 단언. 연속 인계 단위 시험과 실제 trace 전부 통과. |
| 4 인계 성공·실패 별도 수집 | 통과. 위 표. Held-worker 시험에서도 성공 1·실패 1 확인. |
| 5 thread 상한·생성 | 통과. R=1, 16코어에서 허용 17개. 일반 trace peak S/C는 serial 2/2, send 3/2, window 3/2; held-worker peak 17. Dispatcher는 각 process 1개, `try_assign_role` 안의 worker 생성은 모두 0회. |
| 6 전환·완료 context | **부분 통과.** Serial I4 0.0001·E5 0.0037로 목표 충족. Send I4 0.6286은 ≤0.1 미달, E5 0.0474. Window E5 0.9029는 strict one-first 정책의 잔여. 직접 실행 callback의 dispatcher flag·thread id를 계측. 추가 `head-continuation-progress.log`는 server latch 해제 전에 이미 대기 중인 HOLD request의 continuation에서 dispatcher flag와 source thread id 일치를 명시적으로 단언해 통과했다. |
| 7 모든 worker busy fallback | 통과. 실제 held-worker 시험의 E5 1건이 dispatcher로 전환했고 수신·관리도 진행. 단위 시험은 실패 시 holder thread 유지와 기존 queue 실행을 확인. |
| 8 인계 중 stop·미완료 operation | 통과. ≥1,000회 연속 인계 후 성공 수=실행 수·worker 0·join 확인. 실제 host는 callback을 붙잡고 pending operation을 남긴 채 stop 요청, callback 2개·shutdown terminal·pending 0·stop 반환을 확인. Registry는 수집 slot이 남으면 shutdown이 기다리고 모두 게시한 뒤 끝나는 것을 확인. |
| 9 기존 계약 | 통과. 전체 unit·contract 157/157. 기존 registry nested cancel·새 turn, owner FIFO·gate·permit·execution·handler_registry·channel 사용처 포함. 역할 없는 executor의 wait·retire·restart·internal drain도 별도 확인. |

역할 단위 시험·registry 시험·실제 host 회귀 시험은 최신 일반 빌드에서 각각 20회 통과했다. 새 역할·실제 host 회귀 시험은 TSan에서도 각각 20회 통과했고 보고 0건이다. 새 시험에는 sleep이 없다.

## 검증

- `ctest --test-dir c1672h/build -L '^(unit|contract)$' --output-on-failure`: **157/157 통과** (`logs/step2-unit-contract.log`).
- 일반·TSan 추가 빌드: `ninja: no work to do` 확인.
- clang-format 18.1.3: 변경 C++ 파일 12개 통과 (`clang-format18-final.log`). `git diff --check` 통과.
- 양방향 검사: 인계 강제 실패 변형은 handoff 단언에서 실패(`role-red-final.log`, exit -6), collector 비활성화 변형은 수집 개수 단언에서 실패(`registry-red.log`, exit -6).
- 완료 전용 회전: ordinary count가 0이어도 만료·poll이 E5 완료를 만들 수 있다(`public_host_runtime.cpp:6136,6156`). 기존 I0 대기 조건 한 곳에 `!ready && collected.empty()`를 포함했다. 배치 판정은 `try_assign_role` 한 곳으로 유지한다. I0 판정 1→1, 배치 판정 1→1이다.
- 같은 실제 host 회귀 시험에서 I0 guard만 되돌린 변형은 `terminal.wait_for(1s)` 단언으로 실패했다(`completion-negative.log`, exit -6). 수정본은 추가 외부 wake 없이 완료되고 20회 통과했다. Probe 코드는 production source에 없다.

## TSan base/head

전용 `c1672h/tsan-core`, `tsan-binding`, `tsan-pkg`, `tsan-base`, `tsan-head`를 사용했다. Core·binding은 RelWithDebInfo 계측 artifact, Framework는 `-O1 -g -fsanitize=thread`다. Core·binding source는 base와 동일하다. Core의 고유 `__tsan_*` 참조 36개·binding archive 26개와 실제 `libtsan`·전용 `libzlink.so` 로드를 확인했다. Suppression은 쓰지 않았다.

기준은 시작 시 main-base `61893d9ae3`의 고정 사본이다. 비교 시 실제 main-base는 `9450eff5ed`이며, 두 commit의 `framework/languages/cpp`, `framework/runtime`, `framework/test`, `core`, `bindings/cpp` 전체 diff가 0이다.

| 항목 | base | head |
|---|---:|---:|
| 전체 비교 대상 | 17개(새 시험 제외) | 19개 |
| m6b 보고 | 79 | 79 |
| m6c 보고 | 8 | 8 |
| execution 보고 | 4 | 4 |
| channel_messaging 보고 | 1 | 1 |
| 초기 원시 보고 합계 | 92 | 92 |
| 원인 분리 실행을 포함한 원시 보고 | 111 | 92 |
| 정규화한 고유 SUMMARY 위치 | 49 | 46 |
| head 전용 위치 | — | **0** |

양쪽의 m6b·m6c·execution·channel_messaging은 sanitizer exit로 실패했다. Head의 나머지 15개는 통과했다. Base owner_rejection은 terminal assertion 실패, fixture 복원 후 m6a는 timeout이었고 head의 두 시험은 통과했다. Alias r6_creation_terminal은 실제 실행 파일을 빌드한 뒤 양쪽에서 통과했다. **TSan 전체 통과를 뜻하지 않는다.**

보고 비교는 binary 경로·주소·BuildId를 정규화하고 unchanged source 줄을 base로 매핑한다. Optional destructor의 양쪽 read/write source 줄도 대조했다. 남은 차이 `Core poller_api.cpp:46` heap-use-after-free는 base m6c 전체 실행에서 재현했다(`tsan-base-poller-full-3.log`). Relocation-only 5회는 보고가 없었고, 전체 시나리오 3회는 모두 sanitizer 실패했으며 3회째 같은 위치·종류를 포착했다. 통과한 재실행을 결함 부정 근거로 쓰지 않았다. `tsan-sites.json`, `tsan-accesses.json`과 원본 log를 보존한다.

## 결정 필요 항목

**Send I4의 ≤0.1 예상이 실제 per-record 전환과 다르다.** `mesh_node_host_service.cpp:3170–3191`의 기존 owner drain은 새 receiver가 받은 추가 record도 계속 실행한다. `service_mailbox.cpp:232–260`의 draining owner는 매 회전 ready로 다시 배치되지 않는다. 따라서 owner turn의 게시 수가 적어도, 그 뒤에 받은 record는 receiver와 drain worker의 thread가 달라진다. 최종 send는 인계 성공 9·실패 0이면서 I4 전환 6,286건이었다.

감독자 선택지는 다음과 같다. 이 job은 추가 runtime 정책을 구현하지 않았다.

1. Request-serial 목표 달성을 채택하고 설계 A의 send 잔여 전환을 기록한다.
2. Send도 per-record ≤0.1이 필수라면, active owner drain과 이후 수신 회전 사이의 실행 위치를 감독자가 별도로 설계한다. 현재 owner/gate/turn 규칙을 바꾸는 작업이므로 본 job에서 조건·새 상태를 추가하지 않는다.

Window·backpressured send의 나머지 완료를 먼저 dispatcher에 넘기는 strict one-first 결정과 ClientServer 범위 제외는 그대로다. Window에서 allocation·mutex가 늘어난 것도 위 표에 기록했다. 시간 성능 판단은 하지 않는다.

## 변경 파일과 점검

- `dispatch/offload_executor.{hpp,cpp}`: 역할 우선 취득·idle 예약·application/role drain.
- `mesh/mesh_node_host_service.{hpp,cpp}`: node별 thread 제거·수신 역할·I4/E5 경계·stop 순서.
- `mesh/service_mailbox.{hpp,cpp}`: ready owner 반환, 바인딩하지 않은 수동 mailbox의 기존 ready 보존.
- `foundation/operation_registry.{hpp,cpp}`: 기존 intrusive item·drain slot 수집과 공유 callback 실행부.
- `CMakeLists.txt`, `test_cpp_framework_receive_role.cpp`, `test_cpp_framework_receive_role_completion.cpp`, 기존 `test_cpp_framework_operation_registry.cpp`·`test_cpp_framework_m6a_runtime.cpp`.

리팩토링 점검: 성능 1건(vector owner별 이동을 없애고 optional deque의 O(1) swap·빈 회전 할당 0), POSDDD/수명 1건(mailbox ready callback의 Host 참조 제거·기존 독립 capture 사용), 불필요 include 2건 발견·수정. 이 점검에서 넘긴 항목 0. Gate를 중복 보호하는 lock·queue·권한 flag를 추가하지 않았다. 남은 검증 실패는 위 TSan 기준 보고와 send I4 기준 미달이다.

## 산출물과 재현

모든 경로는 `/root/jobs/fx/cx/c1672h` 아래다. 공유 `~/.cache/zlink`에는 쓰지 않았다.

- `harness/driver.cpp`, `trace.hpp`, `instrument.py`, `build-driver.py`, `run.py`, `progress.py`: public typed JSON driver와 별도 source-copy 계측.
- `harness/{base,step1,head}-{plain,trace}`·`harness/out/`: 단계별 binary·원시 횟수. Callback이 await 등록 전에 이미 완료되면 caller 진행도 전환으로 센다. Callback 진입 자체의 dispatcher flag는 trace에서 검사한다.
- `harness/continuation-driver.cpp`·`head-continuation`, `logs/head-continuation-progress.log`: 기존 trace object를 재사용해 실제 in-flight request의 동일 thread continuation을 단언한 별도 검증 binary. 주 횟수 표의 binary는 바꾸지 않았다.
- `scripts/count-table.py`, `scripts/tsan-sites.py`, `scripts/run-tsan.sh`: 표·비교 재현. `scripts/run-head-counts.py`는 최종 세 시나리오를 각각 plain/trace로 실행한다.
- `logs/step2-unit-contract.log`, `tsan-{base,head}.log`, `tsan-base-poller-full-3.log`, `tsan-head-roles-repeat.log`, `head-progress.log`, `count-table.md`, `counts.json`, `tsan-sites.json`, `tsan-accesses.json`.
- 일반 package `pkg/`, 계측 package `tsan-pkg/`는 전용 실제 디렉터리다. Worktree는 미머지 commit 검토를 위해 유지하고 시험 프로세스는 종료했다.
