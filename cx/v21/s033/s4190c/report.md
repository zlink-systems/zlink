# #4190 C/C++ 제출 스레드

## 결과

- C raw와 C++ raw·Framework·gRPC에 stream별 전용 제출 스레드를 적용했다. backpressure/send는 기본 8개, serial은 1개이며 기존 연결 하나를 공유한다.
- 완료는 기존 자원(C poller, C++ binding poller, Framework dispatcher, gRPC CompletionQueue)에서 처리한다. stream별 counter·in-flight·latency·오류를 보호하고 phase 종료에 terminal 집계를 한 번 병합한다.
- trigger와 결과는 `submitConcurrency`를 쓴다. 모든 패턴에 stream 수와 stream별 in-flight를 기록한다. 정규화는 과거 `sendConcurrency` 결과도 읽으며 과거 실제 stream 수는 보존한다.
- 제품 코드와 규격 문서는 수정하지 않았다. 커밋: `bd3b61a14b` (`bench/4190-s4190c`). push하지 않았다.

## 판정 위치 전/후

아래 경로는 `framework/bench/grpc/` 기준이다.

| 결정 | 전 | 후 |
|---|---|---|
| C 제출 스레드 수 | `c/zlink/bench_zlink_client.cpp:218,290,398` 패턴별 단일 루프 | 같은 파일 `:296`에서 수 결정, `:316`에서 전용 thread 생성 |
| C++ 제출 스레드 수 | `cpp/client/bench_cpp_client.cpp:273,564,1039` 행별 단일 루프 | 같은 파일 `:266`의 공통 `stream_count()`; thread 생성 `:324` |
| C sequence | `c/zlink/bench_zlink_client.cpp:233,305,417` 단일 증가 값 | 같은 파일 `:319`의 stream prefix와 지역 증가 값; 요청별 검증 `:108` |
| C++ sequence | `cpp/client/bench_cpp_client.cpp:282,937,1169` 행별 증가 값 | 같은 파일 `:358,446`의 단일 atomic allocator; 요청별 검증 `:233` |
| C 집계 병합 | `c/zlink/bench_zlink_client.cpp:201` 및 패턴별 지역 집계 | 같은 파일 `:341` active snapshot, `:369` terminal 병합 |
| C++ 집계 병합 | 각 driver가 공유 counter·sampler를 갱신 | `cpp/client/bench_cpp_client.cpp:412` frozen stream의 terminal 병합; 오류 분류 `:141` 재사용 |

수정 전/후 판정 위치 수: 제출 구성 C 3→1·C++ 3→1, sequence C 3→1·C++ 3→1, C++ terminal 병합 3→1. stream별 mutex/CV와 phase latch를 두고 기존 ready queue·slot/free-list·행별 제출 상태를 제거했다.

## 바꾼 것

- `c/zlink/bench_zlink_client.cpp:288`, `c/common/bench_common.hpp:158`: 공통 stream 실행, sequence 검증, active/terminal 분리, metadata와 latency 병합.
- `cpp/client/bench_cpp_client.cpp:261`: 세 행 공통 제출 driver, stream별 gRPC stub과 단일 채널, 제출 CPU 합계, native 완료 경로, 안전한 오류 집계.
- `cpp/common/bench_async.hpp:22`, `cpp/common/bench_common.hpp:206`: 기존 완료 자원을 보존하는 observer, 이전 queue/slot helper 삭제, latency 병합.
- `cpp/CMakeLists.txt:86,179`: source binding 사용 시 설치 package lookup 생략, 제출 스레드 테스트 등록.
- `c/zlink/raw_wire_test.cpp:67`, `cpp/tests/submit_threads_test.cpp:70`, `cpp/tests/framework_completion_test.cpp:53`: 정상/잘못된 sequence, 1/8개 제출 thread, 완료 자원, 오류 병합 검증.
- `cpp/run_local.sh:32`, `tools/tests/test_cpp_rejections.py:26`: source stats가 소유하는 설정을 결과에 기록하고 오염 셀에도 stream metadata·terminal 오류를 보존한다.
- `tools/benchagg/model.py:104`, `tools/benchagg/readers.py:127`: 새/과거 필드 정규화와 C stream metadata 수용.
- `tools/tests/test_normalization.py:49`, `tools/tests/test_runner_common.py:19`: 필드 호환성·잘못된 값·충돌 검출, 세 패턴 trigger 검사.
- `runner_common.sh:10,193`: 요청된 두 치환만 적용했다.
- 빌드 산출물 정리: 자체 `c/build`, `cpp/build`를 삭제했고 워크트리는 유지했다.
- stray 결과 삭제: 없음. C/C++ 대상 경로에 커밋된 stray 결과가 없었다.

## 검증

로그·실행 스크립트·원본 JSON은 모두 이 보고서와 같은 JOB 디렉터리에 있다.

- 빌드: `bash build-job.sh`, 추가 C 링크 `cpp-build-slot.sh bash c-link-retry.sh`. 모든 컴파일은 지정 wrapper와 `-j3`, 워크트리 내부 build를 사용했다. `build-cpp-metadata.log`, `configure-c.log`, `build-c.log`, `build-c-link-retry.log`.
- C 환경: 시스템 의존성이 없어 C++와 같은 vcpkg의 protoc/plugin/static package를 사용했다. 설치 grpc pkg-config가 존재하지 않는 `utf8_range_lib` 이름을 출력하므로 임시 build 디렉터리에 실제 `libutf8_range.a`의 별칭을 두었다. 공유 패키지는 수정하지 않았다.
- Core release 1.24.0의 실제 링크를 확인했다(`runtime-libraries.log`, `runtime-libraries-c.log`). C++는 `ZLINK_FRAMEWORK_CPP_USE_BINDINGS_SOURCE=ON`이며 자체 build의 `framework/bindings-cpp/libzlink_cpp.a`를 생성했다(`build-cpp-final.log`). 설치 binding cache는 사용하지 않았다.
- `ctest --test-dir framework/bench/grpc/c/build -R '^bench_c_' --output-on-failure`: **3/3 통과**. C++ 같은 명령: **6/6 통과**. `unit-{c,cpp}-final.log`, `unit-{c,cpp}-final-details.log`.
- 잘못된 sequence를 넣어 C는 1건, C++은 stream마다 1건씩 거부되는지 확인했고 정상 입력도 완료됐다. Framework native observer 대조군으로 callback 자원을 검증했다. 최초 fixture의 producer-thread 가정은 spec의 native dispatcher 계약에 맞게 수정했다.
- `python3 -m unittest discover -s framework/bench/grpc/tools/tests`: **127개 통과**(`tools-tests-final.log`). 추가 정규화 29개·runner 14개·C++ controls 13개 통과. 정상/과거 값은 수용하고 잘못된 값·충돌·설정 누락은 거부했다.
- `bash -n framework/bench/grpc/{cpp/run_local.sh,runner_common.sh}`, `git diff --check` 통과.
- 실행 명령: `IMPLEMENTATIONS=zlink-cpp,zlink-framework-cpp python3 smoke.py cpp`, `python3 smoke.py c`. 실제 `run_local.sh`를 `flock -o /root/jobs/fx/cx/measure.lock`에서 실행한다. payload 1024, warmup 5초, active 5초, run 1.
- 최종 C++ raw·Framework **6셀 통과**: 완료 양수, 오류 0, abandoned 0, `submitConcurrency=8`, stream 수 serial 1·나머지 8, stream별 in-flight serial/send 1·backpressure null. `smoke-cpp-summary.log`, `smoke-cpp/run1/*/results.json`.
- 최종 C raw **3셀 통과**: serial 완료 80158·backpressure 729873·send 2584010, 오류 모두 0. metadata·stream별 in-flight도 같은 계약으로 확인했다(`smoke-c-summary.log`, `smoke-c/run1/*/results.json`). 필수 smoke는 합계 **9셀 통과**이며 성능 수치의 판정은 하지 않았다.
- `/proc/<pid>/task/*/comm` 증거: `threads-c.txt`의 C PID 145644에서 backpressure active의 제출 스레드 8개를 확인했다. `threads-cpp.txt`의 raw PID 131918·Framework PID 134343에서 backpressure active에 `bench-submit-0`~`7` 8개씩 확인했다. `threads-grpc-final.txt`의 gRPC PID 135839도 warmup에서 8개였다.
- gRPC 최종 backpressure 진단은 오염을 재현했고 설정·오류 보존도 확인했다(`smoke-grpc-final-summary.log`). 제출 635690 = 완료 170208 + 오류 396013 + 잔여 69469. 오류는 gRPC status 4, `Deadline Exceeded`였다. 이 실행은 정상 smoke 통과로 세지 않았다.

리팩토링 점검: 성능 1건, POSDDD 2건, 불필요 코드 2건 발견. 단일 제출자 병목, 행별 제출 구성·집계 중복, 이전 ready queue·slot 상태를 고쳤다. 넘긴 항목 없음.

## 남은 문제

- gRPC backpressure는 최종 코드에서도 warmup drain 상한과 deadline 오류로 오염된다(`smoke-grpc-final/`). gRPC serial/send는 오류 0으로 통과했다(`smoke-cpp-cq-model.log`). 규격의 무제한 제출을 유지했으며 window·재시도·deadline 증가는 추가하지 않았다.
- GCC observer template의 `-Wsubobject-linkage` 및 외부 gRPC/Abseil deprecated 경고가 남아 있다. 빌드 실패 원인은 아니다.
