# zlink C++ binding Multi PUBSUB hot-path pass 1

## 결론

`bindings/cpp/**`의 library 코드는 바꾸지 않았다. Callgrind와 C/C++ 교차 실행에서 publisher는
병목이 아니었고, subscriber 쪽 C++ 공개 wrapper 비용이 64B와 4096B에서 각각 약 5% 남아 있었다.
하지만 그 비용은 `subscribe_part()`가 계약대로 `topic`, optional source RID, `message_t` ownership을
호출자에게 전달하는 데서 나왔다. 메시지마다 생기는 binding allocation, `std::function`, binding lock,
payload 복사는 관측되지 않았다. 공개 API·ownership·실패 시 기존 output 보존 계약을 유지하면서
가이드의 5% 효과 기준을 충족할 후보가 없어 no-go로 판정했다.

같은 코드로 수행한 요청된 after run은 C 대비 aggregate throughput 120.7%, latency 0.97x였다.
before의 93.3%보다 높지만 코드 변경이 없으므로 library 개선 효과로 귀속하지 않는다.

## 비용 위치와 원인

Callgrind 조건은 C와 C++ 모두 Core 0.17.0 local Release, tcp, 10 clients, 1초, 2 parts이며 64B와
4096B를 각각 실행했다. publisher와 subscriber를 같은 run에서 모두 계측했다. PUBSUB이 lossy이므로
총 Ir은 완료된 message나 native part 호출 수로 나눠 해석했다.

| 위치 | 64B 관측 | 4096B 관측 | 판정 |
|---|---:|---:|---|
| C++ publisher의 runner+binding 비용 | 약 2,643 Ir/message | 약 2,536 Ir/message | C는 약 2,184 / 2,552 Ir/message. 4096B는 동률이고 64B 추가분도 전체 Core publish 비용에 비해 작다 |
| C++ `socket_t::subscribe_part` wrapper | 5,741,586 Ir / 26,715 calls = 215 Ir/call | 432,534 Ir / 2,097 calls = 206 Ir/call | 반복되는 고정 추가 비용의 중심 |
| C++ subscriber의 Core 밖 비용 | 약 3,027 Ir/completed message | 표본 수가 작아 setup 비용 영향 큼 | 64B C는 약 2,077 Ir/message로 C++가 약 950 Ir/message 더 사용 |
| `message_t` 생성·파괴 | ctor 801,450 Ir, dtor 2,245,889 Ir / 26,715 calls(64B) | ctor 62,910 Ir, dtor 183,096 Ir / 2,097 calls | native header init/close와 RAII wrapper 전달 비용. C도 같은 native init/close를 직접 수행 |
| topic 전달 | `std::string::_M_replace` 997,139 Ir / 약 13.4k messages(64B) | 79,331 Ir / 약 1.0k messages | `topic_out_`에 `bench`를 복사하는 공개 결과 계약. SSO라 per-message heap allocation 없음 |
| subscription filter | socket당 setup 1회 | socket당 setup 1회 | hot path 아님 |
| allocation | wrapper hot caller에서 per-message `operator new` 없음 | 같음 | topic은 SSO이고 output wrapper는 stack 객체. 전체 `new`는 setup/Core thread 쪽 |
| lock | 상위 `pthread_mutex_*`는 Core pipe/socket 내부 | 같음 | binding `subscribe_part`에는 lock 없음 |
| receive completion / `std::function` | 해당 symbol 없음 | 해당 symbol 없음 | 동기 PUBSUB receive라 completion owner와 scheduler를 타지 않음 |
| payload copy | binding copy symbol 없음 | 같음 | 관측된 native `msg_t::copy`는 Core가 10 subscribers로 fanout하는 비용 |

프로파일 파일:

- C publisher/subscriber 64B: `/home/hep7hep7/project/zlink-work/c016/profiles/pubsub-pass1/c64.2362398.callgrind`, `c64.2362408.callgrind`
- C++ publisher/subscriber 64B: `/home/hep7hep7/project/zlink-work/c016/profiles/pubsub-pass1/cpp64.2361015.callgrind`, `cpp64.2361024.callgrind`
- C publisher/subscriber 4096B: `/home/hep7hep7/project/zlink-work/c016/profiles/pubsub-pass1/c4096.2362521.callgrind`, `c4096.2362531.callgrind`
- C++ publisher/subscriber 4096B: `/home/hep7hep7/project/zlink-work/c016/profiles/pubsub-pass1/cpp4096.2361467.callgrind`, `cpp4096.2361476.callgrind`

### Publisher/subscriber 분리

Callgrind와 별도로 10 clients, tcp, 3초, 2 parts로 C/C++, C publisher+C++ subscriber,
C++ publisher+C subscriber를 교차 실행했다. throughput만 비교했다. 두 runner의 timestamp 표현이 달라
교차 실행 latency는 호환되지 않으므로 판정에 쓰지 않았다.

| 조합 | 64B Kmsg/s | C/C 대비 | 4096B Kmsg/s | C/C 대비 |
|---|---:|---:|---:|---:|
| C publisher + C subscriber | 1,094.6 | 100.0% | 919.5 | 100.0% |
| C publisher + C++ subscriber | 1,037.1 | 94.7% | 868.3 | 94.4% |
| C++ publisher + C subscriber | 1,091.3 | 99.7% | 928.9 | 101.0% |
| C++ publisher + C++ subscriber | 1,002.9 | 91.6% | 857.4 | 93.2% |

따라서 잔여 차이는 subscriber 쪽에서 나온다. 64B C publisher의 auto-HWM 결과는 1,048,576이고
C++ publisher는 4,096,000이라 완전히 같은 derived HWM은 아니지만, 4096B는 양쪽 모두 4,096,000이고
같은 방향을 보였다.

교차 실행 report:

- `/home/hep7hep7/project/zlink-work/c016/perf-results/multi/report/perf_c_multi_linux_20260905_054459_c_native_matrix.txt`
- `/home/hep7hep7/project/zlink-work/c016/perf-results/multi/report/perf_cpp_multi_linux_20260905_054445_cpp_native_matrix.txt`
- `/home/hep7hep7/project/zlink-work/c016/perf-results/multi/report/perf_cpp_multi_linux_20260905_054413_cross_cpub_cppsub.txt`
- `/home/hep7hep7/project/zlink-work/c016/perf-results/multi/report/perf_cpp_multi_linux_20260905_054428_cross_cpppub_csub.txt`

## §2.1 / §2.4 판정과 변경

| 후보 | 측정 근거 | 판정 |
|---|---|---|
| 즉시 publish의 operation state/topic staging 제거 | pooled state 재사용, short topic SSO, 4096B publisher가 C와 동률 | 미채택. builder가 입력 topic 수명과 ownership을 소유해야 함 |
| output `message_t`/part wrapper pool | wrapper는 stack이고 native init/close는 C에도 존재 | 기각 목록의 public wrapper pool에 해당하므로 시도하지 않음 |
| topic/source metadata 생략 또는 cache 공유 | topic copy는 약 75 Ir/message지만 `subscribe_part`의 공개 output | 미채택. 반환 의미 변경 |
| empty-output 판정에서 native size 조회 제거 | `_has_payload`는 성공한 empty part와 non-empty part를 정확히 구분해야 다음 실패 때 기존 output을 보존할 수 있음 | 미채택. 조회를 뒤로 옮길 수는 있지만 없애면 ownership/error contract가 약해짐 |
| receive completion/`std::function`/binding lock 제거 | 동기 PUBSUB 경로에 존재하지 않음 | 대상 아님 |
| perf runner scheduler/drain/fairness 수정 | library 비용과 분리해야 함 | 시도하지 않음 |

변경 파일: 없음. `bindings/cpp/include/zlink/**` diff도 0줄이다.

## Before / after / paired C

after 명령은 요청문과 동일하게 실행했다. Core runtime은
`/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.17.0`, clients 100, duration 5초,
runs 1, tcp, 2 parts다.

| Size | paired C Kmsg/s | C++ before | C++ after | after/before | before/C | after/C | C latency ms | after latency ms | after/C latency |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 64 | 626.9 | 507.3 | 738.9 | +45.7% | 80.9% | 117.9% | 1415.674 | 1718.475 | 1.21x |
| 256 | 590.6 | 641.9 | 883.7 | +37.7% | 108.7% | 149.6% | 1817.186 | 1812.345 | 1.00x |
| 1024 | 711.5 | 689.2 | 935.6 | +35.8% | 96.9% | 131.5% | 1148.044 | 920.048 | 0.80x |
| 4096 | 655.2 | 566.6 | 785.6 | +38.6% | 86.5% | 119.9% | 352.786 | 333.603 | 0.95x |
| 65536 | 65.8 | 61.7 | 55.6 | -9.9% | 93.8% | 84.5% | 185.156 | 167.796 | 0.91x |
| 평균 | — | — | — | — | 93.3% | **120.7%** | — | — | **0.97x** |

- C 기준: `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260905_035313_p1cpp.txt`
- C++ before: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260905_035341_p1cpp.txt`
- C++ after: `/home/hep7hep7/project/zlink-wt-cpp-pubsub/bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260905_054800.txt`

after report는 status complete, success 5, fail 0, expected/actual result lines 25/25다. Aggregate 목표
95%와 latency 상한 2.0x는 통과했다. 65536B 개별 비율 84.5%는 개별 최소 85%보다 0.5%p 낮다.
코드가 같은데 before 대비 변동이 매우 크므로 재측정으로 값을 고르지 않았고, after 상승을 library
개선으로 주장하지 않는다.

## Gate

| Gate | 결과 |
|---|---|
| `ZLINK_CORE_SOURCE=local ZLINK_CPP_CORE_BUILD_DIR=$PWD/core/build ZLINK_BUILD_JOBS=4 bash bindings/cpp/tests/run_tests.sh` | PASS: contract 16/16 |
| samples (위 script가 이어서 실행) | PASS: sample smoke 7/7, PUBSUB sample 포함 |
| 관련 `test_cpp_contract_socket` 5회 | PASS 5/5. samples configure 뒤 CTest 등록은 sample 전용이라 남아 있는 contract binary를 직접 5회 실행 |
| `git diff --check` | PASS |
| 공개 header signature | 변경 없음 |
| tracked worktree diff | 없음 |

## BLOCKERS

- 작업 완료를 막는 blocker는 없다.
- 성능 귀속에는 blocker가 있다. 채택한 코드 변경이 없고 동일 코드의 before/after 단일 run 편차가
  35.8~45.7%(64~4096B), -9.9%(65536B)라 after 상승을 library 효과로 합산할 수 없다.
- 65536B after/C 84.5%는 개별 최소 85%에 0.5%p 못 미친다. Aggregate 목표는 120.7%로 통과한다.
- 교차 runner의 latency timestamp는 호환되지 않아 교차 실행에서는 throughput만 유효하다.
