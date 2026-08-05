# 2026-08-04 bindings perf의 .NET 기준 정렬과 smoke 확인

이 기록은 `doc/perf`의 공통 측정 의미를 기준으로 bindings perf를 점검한 결과다.
이번 실행은 full matrix나 C 대비 성능 판정이 아니라, 현재 Core runtime에 연결된
각 runner가 한 가지 pattern과 message size를 끝까지 측정하는지 확인하는 smoke다.

## 공통 실행 조건

- Core version: `11.2.0`
- runtime: `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.0.2.0`
- runtime SHA-256: `ce28d7908bf62a1b39b481aad2a76c6e76955e3a93ea73e1cbdaa913c4883138`
- source freshness: `core/src`와 `core/include`에서 runtime보다 새 파일 0개
- single smoke: `PAIR / inproc / 64B / 1초 / 1회`
- multi smoke: `DEALER_DEALER` 또는 `DEALER_ROUTER / tcp / 64B / 1초 / 1회`
- HWM: 수동 override 없이 context auto-HWM

모든 smoke는 `throughput`, `bandwidth`, `latency`, `latency_p95`, `latency_p99`의
필수 5개 결과를 생성해야 complete로 판정했다.

## smoke 결과

| Binding | Single | Multi | runtime 조건과 판정 |
|---|---|---|---|
| C | `complete`, 5/5 | `complete`, 5/5 | `core/build` 11.2.0, 기준 runner |
| C++ | `complete`, 5/5 | `complete`, 5/5 | `core/build` 11.2.0 |
| .NET | `complete`, 5/5 | `complete`, 5/5 | `core/build` 11.2.0 |
| Rust | `SMOKE PASS` | `SMOKE PASS` | `core/build` 11.2.0, 개발 runtime 경로 |
| Node.js | `complete`, 5/5 | 이번 라운드에서 실행하지 않음 | `core/build` 11.2.0 |
| Python | `complete`, 5/5 | `complete`, 5/5 | `core/build` 11.2.0 |
| Go | `complete`, 5/5 | 이번 라운드에서 실행하지 않음 | 공식 native package는 11.1.0 |
| Java | 실행 전 실패 | 실행 전 실패 | 승인된 Core 11.2 package prefix 없음 |

Go의 공식 smoke는 `bindings/go/native/linux-x86_64/libzlink.so.0.1.0`을 사용해
결과를 만들었다. 따라서 실행 성공은 확인했지만 현재 11.2.0 Core와 같은 runtime을
사용한 결과로 보지 않는다. 같은 Core runtime을 package 모양의 임시 경로에 연결한
개발용 진단에서는 single `PAIR / inproc / 64B`가 통과했지만, 이는 package 배포 검증을
대체하지 않는다.

Java는 benchmark process를 시작하기 전에 `bindings/java/build.gradle`의
`ZLINK_CORE_PACKAGE_PREFIX` 검사에서 중단됐다. 현재 저장소에는 version `11.2.0`과
provenance를 함께 증명하는 승인 package prefix가 없으므로, 임의 manifest를 만들거나
`core/build`를 설치 package로 가장하지 않았다.

## 확인한 정렬 수정

### .NET multi phase 종료

기존 .NET multi server는 stop token과 일반 message를 같은 `bool` 결과로 처리했다.
stop token이 active deadline 직전에 수신되면 종료 신호가 버려지고 무기한 poll에
진입해 `result_timeout`이 발생했다. 수신 결과를 `NoData`, `Message`, `StopToken`으로
나누고 stop token을 server receive path에서 처리하도록 수정했다. runner에 짧은
poll timeout이나 timer fallback을 추가하지 않았으므로 `PERF_MULTI_TEST_POLICY.md`의
phase 종료 의미를 유지한다.

수정 전에는 `MULTI_DEALER_DEALER / tcp / 64B / clients=4`가 결과 0/5로 timeout했고,
수정 후에는 5/5 complete가 됐다. 수정은 이미 원격 branch의
`5e58064b7d`에 포함되어 있다.

### C++ runtime provenance와 public surface

C++ perf runner는 `ZLINK_CPP_USE_CORE_BUILD_RUNTIME`을 CMake에 전달했지만 CMake가
그 변수를 선언하지 않아, 실제로 `core/build`를 사용해도 최종 guard에서 중단됐다.
다음 두 대안을 검토했다.

1. runtime guard를 제거하고 CMake의 자동 target 선택에 맡긴다. 이 경우 다른 Core
   package가 선택돼도 smoke가 통과할 수 있어 runtime provenance가 약해진다.
2. CMake가 해당 option을 소유하고 `ON`이면 `core/build` runtime만 허용한다.

호출자가 확인해야 할 경로 정보를 runner와 CMake에 중복해서 두지 않도록 2번을
선택했다. 이 변경과 현재 C++ public `received_t`에 없는 `spot_rid()` 호출 제거는
`f193d2e885`에 커밋하고 원격 branch에 push했다.

## 정책 판정

다음 항목은 smoke 범위에서 확인됐다.

- 실제 runtime 경로를 출력하고 stale Core source를 실행 전에 검사한다.
- 기본 HWM은 auto-HWM이며 message unit은 64B smoke에서 64B로 확인됐다.
- 결과는 공통 `RESULT` 형식과 필수 5개 metric을 사용한다.
- .NET, C++, Rust, Node.js, Python의 data path는 각 binding public API를 사용한다.
- .NET multi phase는 C 기준의 stop-token 의미를 따른다.
- 34개 perf shell script의 `bash -n` 검사는 모두 통과했다.

아직 “모든 binding이 .NET과 동일하다”라고 판정할 조건은 남아 있다.

- Java는 승인된 11.2.0 package provenance가 없어 실행되지 않았다.
- C, Go, Rust의 vendored `zlink.h`는 11.1.0이며 현재 Core 11.2.0과 header/runtime
  version provenance가 맞지 않는다. Go의 공식 native package도 11.1.0이다.
- Python은 auto-HWM 세부값을 `unavailable`로 출력하고, Node.js는 socket buffer
  세부값을 `?`로 출력한다. 결과 complete와 별개로 C 대비 정식 성능 비교에 필요한
  HWM detail 증거는 보강해야 한다.
- `--smoke` convenience option은 Python, Go, Rust에는 있지만 C, C++, .NET,
  Java, Node.js에는 없다. 후속 공통 CLI 계약을 정할 때 정리할 항목이며, 이번에는
  C와 .NET 방식처럼 pattern, transport, size를 한 셀로 지정하는 방식으로 확인했다.
- full matrix, 반복 중앙값, C 대비 throughput/latency gate는 이번 smoke의 범위가
  아니므로 완료 판정에 사용하지 않는다.

따라서 현재 결과는 “fresh Core 11.2 runtime을 사용하는 주요 binding smoke는 통과”이며,
“전체 binding perf parity와 release package smoke가 완료”인 상태는 아니다.
