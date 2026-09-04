# zlink C++ binding hot-path 개선 pass 2 진행 로그

- 시작: 2026-09-05 (Asia/Seoul)
- 작업 트리: `/home/hep7hep7/project/zlink-wt-cpp-perf2`
- 기준: detached `ee50ebaeaf0e`
- 범위: 구현·테스트 변경은 `bindings/cpp/**`만 허용. `core/build`, `core/build-dev` symlink는 기존 untracked 상태로 보존.
- 제한: `core/**` configure/build/clean 금지, Git 상태 변경 명령 금지, build jobs 최대 4.

## 1. 시작 상태와 기준 확인

- `git branch --show-current`: 빈 값(detached HEAD).
- `git status --short`: 기존 untracked `core/build`, `core/build-dev`만 존재.
- 계획서 §7.4 9~11과 §8, 최적화 가이드 §2·§4, pass 1 summary를 확인했다.
- pass 1 잔여 비용: REQREP 1024B 기준 49.16k Ir/op, 10.44 `new`/op; coroutine scheduler와 `std::function` 각 1회/op, `message_t` move 5회/op, completion map node와 request/reply wrapper 생성.

상태: read-only source review 진행 중.

## 2. Read-only 후보 판정

| 후보 | 계약 보존 근거 | 예상 효과 | 판정 |
|---|---|---|---|
| reply가 먼저 끝난 경우 coroutine suspend 생략 | `async_result_t::awaiter_t::await_ready()`와 `async_operation_state_t::suspend()`의 terminal 재검사가 이미 race 양쪽을 처리한다. | 이미 적용되어 추가 효과 없음 | no-go(중복) |
| scheduler ABI를 함수 포인터/고정 task로 교체 | public header의 `async_continuation_scheduler_t = std::function<...>`와 custom promise 연동 계약을 바꾼다. | 간접 호출·manager 비용 감소 가능 | no-go(공개 signature/ABI) |
| resume slot을 operation bundle 안에 두고 aliasing lifetime으로 보호 | operation마다 새 bundle identity를 유지하고, queued callback은 같은 shared control block을 잡는다. completion/awaiter의 abandon 경쟁은 atomic slot으로 유지한다. public scheduler signature는 그대로다. | suspended REQREP당 `operator new` 1회 제거 | 채택 |
| 2-part request staging inline화 | native staging은 이미 8-part stack 경로이고, builder의 두 vector capacity는 pooled operation state에서 재사용된다. callgrind에서 `dealer_socket_t::request()`의 state allocation은 1,405 op 중 1회뿐이다. | 정상 상태 이득 없음 | no-go(중복·복잡도) |
| reply vector를 resize한 뒤 native part를 제자리 adopt | public 반환형·part 순서·native ownership은 같고, 임시 `message_t`를 vector로 옮기는 단계만 없앤다. | 2-part 측정에서 `message_t` move 2회/op 제거 | 채택 |
| completion map의 단일-entry inline slot | socket당 동시 요청 1개는 inline shared ownership으로 찾고, 둘째부터 기존 PMR map을 그대로 사용한다. 등록·lookup·해제는 같은 owner mutex 아래이고 callback identity는 재사용하지 않는다. | REQREP register/unregister hash·node 경로 약 0.5~1.0 kIr/op 절감 예상 | 채택 |
| completion node pool 확대/entry pool | PMR node pool은 socket lifetime 동안 이미 재사용한다. entry/Future identity pool은 늦은 completion ABA 위험 때문에 금지 대상이다. | 추가 allocation 감소 근거 없음 | no-go |
| perf REQREP request/reply 생성 변경 | C/C++ 모두 client당 1 outstanding, 같은 payload 크기, 같은 1/2 part 설정, payload→native message 복사 1회를 사용한다. | parity 변경 없이 줄일 중복 없음 | no-go(측정 의미 보존) |

상태: 위 세 내부 후보 구현 시작.

## 3. 구현 후 좁은 검증과 callgrind

- 관련 4종 `test_cpp_contract_request_reply`, `test_cpp_contract_request_writable_retry`, `test_cpp_perf_application_ready_queue`, `test_cpp_contract_optimization_guard`: 1회 PASS.
- 10 clients, tcp, 1024B, 1초 DEALER_ROUTER_REQREP 진단 실행: complete.
- callgrind 파일: `/home/hep7hep7/project/zlink-work/c016/profiles/cpp-reqrep-pass2.callgrind`.
- pass 1 after는 1,405 submit/completion, 69,077,105 Ir, 14,662 `operator new`: 49.16k Ir/op, 10.44 new/op.
- pass 2는 1,415 submit/completion(측정 deadline 안 count 1,405), 68,721,654 Ir, 13,336 `operator new`: **48.57k Ir/op, 9.42 new/op**.
- 고정비 변화: Ir/op -1.2%, new/op -9.8%; reply `message_t` move는 5.00 → 3.00/op. 별도 resume-slot control block은 사라졌고 public `std::function` scheduler signature는 유지됐다.

상태: 공식 100-client after 측정 준비.

## 4. 공식 after (100 clients, tcp, 5초, runs 1)

- report: `bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260905_043517.txt`
- status: 15/15 complete, fail/skip/unsupported 0.

| 패턴 | 크기 | pass 1 after | pass 2 after | 변화 |
|---|---:|---:|---:|---:|
| DEALER_DEALER | 64 | 740,751.0 | 743,440.6 | +0.36% |
| DEALER_DEALER | 256 | 711,017.8 | 724,345.8 | +1.87% |
| DEALER_DEALER | 1024 | 722,420.4 | 709,697.4 | -1.76% |
| DEALER_DEALER | 4096 | 335,123.0 | 331,874.4 | -0.97% |
| DEALER_DEALER | 65536 | 104,544.8 | 105,103.2 | +0.53% |
| DEALER_ROUTER_REQREP | 64 | 86,065.0 | 80,275.0 | -6.73% |
| DEALER_ROUTER_REQREP | 256 | 70,955.8 | 81,202.8 | +14.44% |
| DEALER_ROUTER_REQREP | 1024 | 62,216.8 | 73,514.8 | +18.16% |
| DEALER_ROUTER_REQREP | 4096 | 57,051.0 | 58,162.8 | +1.95% |
| DEALER_ROUTER_REQREP | 65536 | 21,354.2 | 20,585.0 | -3.60% |
| ROUTER_ROUTER_REQREP | 64 | 88,867.4 | 89,505.0 | +0.72% |
| ROUTER_ROUTER_REQREP | 256 | 70,545.8 | 61,315.4 | -13.08% |
| ROUTER_ROUTER_REQREP | 1024 | 64,666.8 | 66,712.0 | +3.16% |
| ROUTER_ROUTER_REQREP | 4096 | 59,820.2 | 61,991.0 | +3.63% |
| ROUTER_ROUTER_REQREP | 65536 | 21,953.8 | 22,588.6 | +2.89% |

- size별 pass2/pass1 ratio 산술평균: DD 100.01%, DR-REQREP 104.84%, RR-REQREP 99.46%.
- 단일 공식 run은 셀별 편차가 섞였으나, callgrind에서 고정 allocation과 instruction 감소가 직접 확인됐고 DD 즉시-admission 경로는 중립이다. 추가 반복으로 값을 선택하지 않는다.

상태: 전체 gate 및 관련 contract 5회 진행.

## 5. Gate와 종료

- 전체 `bindings/cpp/tests/run_tests.sh`: PASS(contract 16/16, sample smoke 7/7).
- 관련 4종 contract/perf guard: test 구성을 다시 활성화한 뒤 각각 실제 5회 PASS. sample 전용 reconfigure 뒤의 `No tests were found` 출력은 반복 횟수에 포함하지 않았다.
- `test_cpp_send_close_stress`: PASS.
- `git diff --check`: PASS.
- `bindings/cpp/include` diff: 0. 공개 header signature 변경 없음.
- tracked 변경: `bindings/cpp/src/Runtime/Messaging/**` 5개 파일. 기존 untracked Core build symlink 2개는 그대로다.
- 요약: `/home/hep7hep7/project/zlink-work/c016/cpp-perf-pass2-summary.md`.

EXIT:0
