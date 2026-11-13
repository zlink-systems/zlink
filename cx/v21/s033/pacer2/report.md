# #1639 C++ bench 증거

## 빌드와 거부 입력

- 빌드: `/root/jobs/fx/cx/cpp-build-slot.sh /root/jobs/fx/cx/v21/s033/pacer2/measure/raw/build-source-binding.sh`로 실행, `cmake --build ... --parallel 3`, Release, `ZLINK_FRAMEWORK_CPP_USE_BINDINGS_SOURCE=ON`; **rc 0** ([명령·로그](measure/raw/build-source-binding.sh), [build.log](measure/raw/build.log), [build.rc](measure/raw/build.rc)). 벤치 CMake가 binding package를 먼저 찾는 부분은 소스 바인딩 모드에서만 건너뛰도록 빌드 중 임시 조정했고 원복했다. Framework가 `bindings/cpp` 소스를 직접 빌드했다.
- p50 기록용 출력 필드를 임시 추가해 client target만 다시 빌드했다(**rc 0**; [로그](measure/raw/build-p50-final.log), [rc](measure/raw/build-p50-final.rc)). 소스 변경은 원복했다. 최초 p50 재빌드 시도에서 CMake 재구성이 실패한 로그도 보존했다([로그](measure/raw/build-p50.log), [재시도 로그](measure/raw/build-p50-retry.log)).
- `--target-rate -1`: **rc 2**, 측정 시작 전에 거부 ([결과](measure/raw/reject-minus1.log), [rc](measure/raw/reject-minus1.rc)). `--target-rate nan`: **rc 2**, 측정 시작 전에 거부 ([결과](measure/raw/reject-nan.log), [rc](measure/raw/reject-nan.rc)).
- 네 벤치 실행 파일은 링크 완료 후 존재를 확인했다([build.log](measure/raw/build.log)); 측정 완료 뒤 2 GiB를 넘는 빌드 디렉터리를 지웠다([빌드·정리 기록](measure/raw/build.log)). 작업 브랜치 파일 변경은 없다.

## 22k paced 측정

조건은 `request-backpressure`, 1024 B, `TARGET_RATE=22000`, source CPU `0-7`, target CPU `8-15`다. 순서는 binding→Framework, Framework→binding, binding→Framework다. CPU는 active window 중 `/proc` user+sys tick으로 계산했고, thread 합은 before/after TID 합집합을 사용했다. `only 1 snapshot`은 합집합 중 한쪽 snapshot에만 나타난 TID 수(source/target)다. CPU ratio는 source와 target의 process µs/msg 합계, Framework ÷ binding이다.

| 회차 | 구현 | 달성 msg/s | p50 / p99 ms | process CPU source/target µs/msg | TID 합 CPU source/target µs/msg | 한 snapshot만 source/target TID | Framework÷binding CPU | 원본 |
|---|---|---:|---:|---:|---:|---:|---:|---|
| 1 | binding | 21,991.1 | 0.187 / 0.202 | 23.7 / 21.4 | 23.6 / 21.5 | 0 / 0 | — | [results.json](measure/captured/round1-binding/run1/zlink-cpp-request-backpressure-1024/results.json), [metrics.json](measure/captured/round1-binding/run1/zlink-cpp-request-backpressure-1024/capture/metrics.json) |
| 1 | Framework | 21,322.3 | 0.285 / 0.381 | 80.3 / 70.0 | 80.3 / 70.0 | 0 / 1 | 3.330 | [results.json](measure/captured/round1-framework/run1/zlink-framework-cpp-request-backpressure-1024/results.json), [metrics.json](measure/captured/round1-framework/run1/zlink-framework-cpp-request-backpressure-1024/capture/metrics.json), [binding metrics](measure/captured/round1-binding/run1/zlink-cpp-request-backpressure-1024/capture/metrics.json) |
| 2 | Framework | 21,256.9 | 0.285 / 0.379 | 85.0 / 66.9 | 85.0 / 66.9 | 0 / 0 | 3.565 | [results.json](measure/captured/round2-framework/run1/zlink-framework-cpp-request-backpressure-1024/results.json), [metrics.json](measure/captured/round2-framework/run1/zlink-framework-cpp-request-backpressure-1024/capture/metrics.json), [binding metrics](measure/captured/round2-binding/run1/zlink-cpp-request-backpressure-1024/capture/metrics.json) |
| 2 | binding | 21,982.9 | 0.186 / 0.216 | 22.2 / 20.4 | 22.2 / 20.3 | 0 / 0 | — | [results.json](measure/captured/round2-binding/run1/zlink-cpp-request-backpressure-1024/results.json), [metrics.json](measure/captured/round2-binding/run1/zlink-cpp-request-backpressure-1024/capture/metrics.json) |
| 3 | binding | 21,984.6 | 0.187 / 0.209 | 20.7 / 21.9 | 20.7 / 21.9 | 0 / 0 | — | [results.json](measure/captured/round3-binding/run1/zlink-cpp-request-backpressure-1024/results.json), [metrics.json](measure/captured/round3-binding/run1/zlink-cpp-request-backpressure-1024/capture/metrics.json) |
| 3 | Framework | 21,372.2 | 0.283 / 0.375 | 81.9 / 68.2 | 81.9 / 68.0 | 0 / 1 | 3.524 | [results.json](measure/captured/round3-framework/run1/zlink-framework-cpp-request-backpressure-1024/results.json), [metrics.json](measure/captured/round3-framework/run1/zlink-framework-cpp-request-backpressure-1024/capture/metrics.json), [binding metrics](measure/captured/round3-binding/run1/zlink-cpp-request-backpressure-1024/capture/metrics.json) |

paced 셀의 latency에는 pacer sleep이 들어가므로 조건값으로 읽는다. pacer 동작 모양은 이전 xio pacer와 달라 1:1 비교 대상이 아니다.

## Unpaced 확인

| 구현 | 설정 rate | 달성 msg/s | p50 / p99 ms | 원본 |
|---|---:|---:|---:|---|
| binding | 0 (미설정) | 227,012.5 | 0.601 / 13.956 | [results.json](measure/captured/unpaced-binding/run1/zlink-cpp-request-backpressure-1024/results.json), [metrics.json](measure/captured/unpaced-binding/run1/zlink-cpp-request-backpressure-1024/capture/metrics.json) |
| Framework | 0 (미설정) | 47,452.2 | 13.145 / 20.496 | [results.json](measure/captured/unpaced-framework/run1/zlink-framework-cpp-request-backpressure-1024/results.json), [metrics.json](measure/captured/unpaced-framework/run1/zlink-framework-cpp-request-backpressure-1024/capture/metrics.json) |

두 unpaced 셀 모두 metadata의 `targetRate=0`이며 기존 제출 경로를 사용한다. 전체 실행 순서와 캡처 없는 최초 시도의 원본도 보존했다([측정 순서](measure/raw/measurement-order.log), [최초 시도 로그](measure/raw/measurement.log)); 위 표는 CPU 캡처가 포함된 `measure/captured/` 결과다.
