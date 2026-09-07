# G-7 gate summary

## 결과

- G-7 patch(`core/src/runtime/core/mailbox.cpp`, 16 insertions/4 deletions)를 main에 `git apply --3way`로 충돌 없이 적용했다.
- release lib-only(`JOBS=4`)를 러너보다 먼저 최신화했고, dev와 hotpath build도 `JOBS=4`로 완료했다. 빌드 전 ninja 수는 각각 0이었다.
- public interface diff(`core/include`, `core/src/libzlink.vers`)는 적용 전후 모두 비어 있었다. `git diff --check`도 통과했다.
- mirror 절차 문서는 현재 8-header 절차를 포함하지 않아 fallback으로 c/cpp/go/rust의 `zlink.h`, `zlink_enum.h`, `zlink_errno.h` 12개 `cmp`를 실행했고 모두 일치했다.

## 테스트

| 항목 | 결과 |
|---|---|
| 전체 dev ctest (`-j2`) | 208/208 PASS |
| focused `wake|poll|mailbox|signaler|stream|pipe|close|release|dealer|router|pair` | 56/56 PASS × 5 |
| lost-wake set (`until-fail:20`) | `wake_invariants`, `two_poller_wake`, HWM/owner invariants: PASS |
| `test_two_poller_wake` 추가 ×20 | PASS |
| `test_wake_invariants` 추가 ×20 | PASS (약 30초/회) |
| `stream|pipe` (`-j4`) | 23/23 PASS |
| `test_close_completion_poller_release` ×50 | PASS |

알려진 간헐 테스트 `test_stream_socket_recv_multiclient_ready_regression`도 위 전체·focused·stream/pipe 실행에서 모두 통과했다. `unittest_request_timeout_scheduler`는 전체 suite의 `-j2` 실행에서 통과했다.

## hotpath gate

`flock $PERF_LOCK` 및 ninja 비실행 상태에서 valgrind callgrind gate를 실행해 종료 성공했다. load average(실행 직전): 4.86, 1.42, 1.15.

| 셀 | instructions/op | reference 대비 |
|---|---:|---:|
| dealer_dealer_inproc | 3230.922 | +0.00% |
| dealer_router_reqrep_inproc | 18261.420 | -3.23% |
| pair_inproc | 2287.784 | -2.58% |
| router_router_tcp | 2873.954 | -3.32% |
| stream_tcp | gate PASS (측정 프로세스 stdout 상한으로 수치 보존 불가) | 예상 약 -1.3% |

## with_stream (local Release lib, runs=1)

load average: 1.89, 1.35, 1.15. `ZLINK_CORE_SOURCE=local`, `zlink,asio`, CCU 1000, mismatch 0.

| size | zlink kops | Phase-0 zlink 기준 | 비율 |
|---:|---:|---:|---:|
| 64 | 315.82 | 268.9 | 117.4% |
| 1024 | 293.23 | 243.0 | 120.7% |
| 65536 | 38.43 | 30.4 | 126.4% |

원칙 전/후: executor 전달에서 public FD 소비자가 없을 때 eventfd write/read 왕복과 asio post라는 두 wake 채널을 사용하던 규칙이, asio post 하나만 사용하는 규칙으로 줄었다. 분류: B 기존 결함(중복 wake 제거). 스펙 문서는 수정하지 않았다.
