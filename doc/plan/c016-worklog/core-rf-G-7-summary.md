# core-rf G-7: async mailbox 중복 eventfd 왕복 제거

- worktree: `~/project/zlink-work/g7` (detached `d17889b981`), 커밋 없음.
- 결과: **부분 성공**. async command owner의 중복 eventfd write/read는 줄였지만 voluntary context switch는 줄지 않았다.
- 소유 계층: 동기화 모델 §3.3의 MPSC mailbox가 command publish와 owner wake를 소유한다. §3.2의 pipe 양 끝·SPSC queue·wake command는 변경하지 않았다.
- 변경 분류: **B — 기존 Core 결함 수정**. asio executor가 실제 owner wake인데 waiter가 없는 primary eventfd도 함께 쓰고 읽던 내부 중복이며 public 계약 변경이나 spec gap은 없다.
- 교차 구현: libzmq v4.3.5 `mailbox.cpp`는 `_cpipe.flush() == false`일 때 한 signaler를 쓰고 같은 receiver가 한 번 읽는다. zlink는 asio post와 private primary eventfd가 같은 owner를 중복 wake하던 점이 달랐다.

## 변경

유일한 소스 변경은 `core/src/runtime/core/mailbox.cpp:56-88,212-227`이다.

- `_io_context && _handler`인 executor mailbox에 public primary descriptor 사용자가 없으면 `_cpipe` sleep→active 전이는 기존 `boost::asio::post`만 사용한다. waiter가 없는 `_signaler.send()`는 생략한다.
- scheduled executor는 그 경우 존재하지 않는 primary signal을 `recv_failable()`로 읽지 않는다.
- `get_fd()`가 설정한 `_primary_signaler_required`가 참이면 primary edge를 전과 같이 쓰고 읽는다. secondary signaler 게시, public poller rearm, blocking mailbox 경로도 그대로다.
- 새 상태·플래그·옵션·helper·public API는 추가하지 않았다. **수정 전/후 규칙 수: executor owner + public descriptor 없음인 경우 wake channel 2(asio post + eventfd) → 1(asio post).**

검토한 대안은 둘이다. primary signaler 자체를 coalescing하고 public poller drain을 한 번으로 합친 안은 write/read가 0.419/0.709까지 줄었지만 mailbox 전체의 primary 의미를 바꾸며 이득도 작아 제외했다. 여기에 scheduled owner의 primary 소비까지 생략한 1차 안은 `test_wake_invariants` 2회차 ROUTER/inproc iteration 86에서 2초 지연으로 실패해 즉시 기각했다. 최종안은 이미 존재하는 asio owner wake만 단일 소유자로 남긴다.

## 측정

Release+LTO, 1024 B, tcp, CCU 20, 10 s 축소셀이다. Callgrind는 `--cache-sim=no`, native 수치는 `/proc/<pid>/io`와 task status이며 모든 측정은 지정 `PERF_LOCK` 아래 실행했다.

| 호출 또는 지표 (/msg) | fresh before | after | 변화 |
|---|---:|---:|---:|
| callgrind `signaler_t::send` / eventfd write | 0.460 | **0.214** | −53.5% |
| callgrind `recv_failable` / eventfd read | 0.795 | **0.546** | −31.3% |
| callgrind `epoll_wait` | 0.508 | **0.514** | +1.2% (동일 수준) |
| native eventfd write | 0.713 | **0.330** | −53.7% |
| native eventfd read | 0.864 | **0.501** | −42.0% |
| native voluntary ctxsw | 0.607 | **0.640** | +5.4% |

G-A 기준치(zlink callgrind write/read 0.439/0.762, `epoll_wait` 0.542; native ctxsw 0.619)는 fresh before와 같은 범위다. after write는 zmq 기준 0.34 아래지만 read 0.34와 ctxsw 0.505에는 도달하지 못했다. `epoll_wait`가 유지되고 ctxsw가 줄지 않은 것은 제거한 eventfd가 waiter 없는 보조 경로였기 때문이다. 남은 switch는 실제 asio post 기반 I/O→app command handoff다.

## 검증

| 검증 | 결과 |
|---|---|
| Release+LTO `release-gate`, dev 빌드 (`JOBS=4`) | 성공; 기존 `unittest_monitor_ready_drain` GCC 경고만 존재 |
| `ctest -R 'wake|poll|mailbox|signaler|stream|pipe|close|release' -j2` | 38/38 × 5회, 총 190 test 통과 |
| lost-wake 핵심 6종 `--repeat until-fail:20` | 각 20회, 총 120회 통과(646.24 s) |
| TSan before/after 동일 6종 | 신규 warning **delta 0** |
| hotpath 5셀 | 모두 PASS |
| with_stream `runs=1`, zlink/asio, 64/1024/65536 B | 완료, 전 셀 mismatch 0 |

이 환경에는 Clang이 없어 저장소 `ENABLE_TSAN`은 LLVM 전용 `-mllvm` 옵션에서 빌드되지 않았다. GCC 13의 완전 `-fsanitize=thread` 계측으로 같은 소스의 before/after를 직접 비교했다. 양쪽 모두 `ypipe.hpp:104` 8건과 `socket_base_msg.cpp:68` 1건, 총 9건이었다. `test_stream_packet_progress`의 `transport did not queue all fragments` 시간 조건 실패도 양쪽 동일했고 나머지 5종은 통과했다. 따라서 알려진 race/계측 지연은 남지만 G-7 신규 경고는 0이다.

hotpath before→after(instr/msg):

| DD inproc | DR reqrep inproc | PAIR inproc | RR tcp | STREAM tcp |
|---:|---:|---:|---:|---:|
| 3271.176→3271.112 | 18658.231→18753.765 | 2330.998→2331.770 | 2916.369→2916.133 | 14377.944→14184.872 |

with_stream before→after(kops, `zlink / asio`): 64 B `285.00/363.09 → 202.84/201.20`, 1024 B `248.67/341.54 → 145.67/186.95`, 65536 B `34.39/42.31 → 19.11/25.80`. 기준 asio도 함께 크게 하락한 단일 런이므로 patch 성능 delta로 해석하지 않고 완료·mismatch 0만 판정 근거로 쓴다.

## 계약 문장 재확인

- **05-polling level:** “등록한 source의 readiness가 거짓에서 참으로 바뀌면 … timeout이 남아 있어도 그 시점에 깨어난다”와 “그 전이를 Core 내부 thread가 처리했더라도 이 보장은 같다”(`05-polling.ko.md:74-77`). public descriptor가 요구된 경로의 primary/secondary wake와 rearm을 그대로 두었으므로 동일하다.
- **socket README recv/wake 조건:** “거절 원인이 되는 자원의 회복만 wake 조건이다 — 규칙 하나”(`socket/README.ko.md:1082-1085`). completion·WRITABLE·credit 판단을 전혀 바꾸지 않았고 command 전달의 물리 채널만 하나로 줄였다.
- **D-079:** 두 poller 중 하나가 mailbox edge를 소비해 다른 waiter가 잠드는 것이 root이며 “poller별 wake channel 복원 OR … 모든 public waiter 재wake”가 수정 방향이다. `signal_registered_pollers_unlocked()`와 public primary edge를 유지해 이 문장과 같은 동작이다.
- **D-099:** “도착한 fragment로 조립”과 “64-chunk bounded step + 기존 receive-progress/mailbox wake로 재무장”을 유지한다. STREAM·pipe·drain 경계는 한 줄도 바꾸지 않았다.
- **S-12:** close 실패는 edge 소비가 아니라 close admission의 EBUSY 경합이며, primary rearm 추가는 기각된다는 결론을 재확인했다. close/lifecycle 코드는 바꾸지 않았고 public primary 경로도 유지했다.

**어느 문장도 다른 동작이 되지 않았다.**

## 멈춘 지점

eventfd 중복 제거까지 반영하고 ctxsw 축소는 멈췄다. 남은 ctxsw는 실제 asio post 기반 owner handoff이며 이를 줄이는 다음 지렛대는 ypipe flush/command batch 경계다. 그 파일은 G-11b 소유 범위이고 G-7에서 보상 규칙이나 추가 상태로 우회하지 않았다. 공개 헤더·`libzlink.vers`·스펙 문서는 수정하지 않았다.
