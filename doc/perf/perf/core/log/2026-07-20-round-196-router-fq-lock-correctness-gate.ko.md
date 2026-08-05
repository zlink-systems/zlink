# Round 196: Router FQ 잠금 수정의 correctness gate와 TLS blocker

## 범위

- runtime: `core/build/lib/libzlink.so.10.6.0`
- runtime SHA-256:
  `a57d91a90ae3c0d67ed7d469df848e70038ff700e93fd5989c889795755b3301`
- 실행 환경: WSL2, Linux, Release
- 대상: `MULTI_SPOT_REQREP`, 64·256바이트, 100 peer, active 1초

공식 `core/build`를 다시 빌드한 뒤 runner가 위 runtime 경로를 출력하는지 확인했다.
`core/src`와 `core/include`에서 runtime보다 새로운 파일도 없었다. 실행 전에는 중지 상태로
보존 중인 기존 runner PID `8508` 외에 다른 성능 측정 process가 없었다.

## TCP 반복 gate

Router의 두 수신 경로를 기존 dispatch 잠금 범위에 포함한 최종 source로 TCP를 20회
반복했다. 64바이트와 256바이트 cell이 모두 완료됐고 process 종료 오류, assertion과
timeout은 발생하지 않았다.

| 크기 | 처리량 중앙값 | mean | p95 | p99 | 결과 |
|------|---------------:|-----:|----:|----:|------|
| 64바이트 | 111,796.5 ops/s | 0.458 ms | 0.839 ms | 2.118 ms | 20/20 통과 |
| 256바이트 | 112,046.5 ops/s | 0.446 ms | 0.822 ms | 2.068 ms | 20/20 통과 |

각 반복에서 hub와 100개 peer의 pending application message, pending infrastructure
message, pending byte와 multicast dropped target은 모두 0이었다.

결과:
`bindings/c/perf/results/multi/report/perf_c_multi_linux_20260720_182631_s9-p02-router-fq-lock-final-tcp.txt`

### Deterministic FQ termination 회귀

성능 runner 반복과 별도로 `ZLINK_BUILD_TESTS`에서만 사용하는 barrier hook을 FQ의 pipe 선택 지점에
설치했다. 수신 스레드를 선택 직후 멈춘 상태에서 같은 pipe의 termination을 시작해 Router dispatch
잠금이 두 동작을 직렬화하는지 검사한다. `xrecv`와 `xrecv_routed` 두 경로의 회귀가 현재 source에서
통과했고 focused 20회 반복과 ASan·UBSan도 통과했다.

같은 source의 별도 RED build에서는 두 수신 함수의 dispatch lock만 제거했다. 기존 회귀 2개는
통과했지만 신규 FQ termination 회귀 2개는 정확히 실패했고 교착 없이 종료 코드 2를 반환했다. 따라서
100-peer 반복 통과만으로 추정하지 않고 FQ 수신 갱신과 pipe 제거의 잠금 경계를 직접 고정했다.

## TLS와 WSS gate

같은 runtime과 부하에서 TLS와 WSS를 각각 5회 실행했다. WSS는 두 크기가 모두 5회
통과했지만 TLS는 매번 첫 64바이트 단계에서 client process가 종료 코드 1로 끝났다.
runner는 그 실패로 다음 256바이트 단계를 실행하지 않았으므로, TLS 256바이트 행은 별도
실행 실패가 아니라 앞선 64바이트 실패를 이어받은 결과다.

| 전송 | 크기 | 반복 결과 | 판정 |
|------|------|-----------|------|
| TLS | 64바이트 | 0/5 | 실패 |
| TLS | 256바이트 | 미실행 | 64바이트 실패로 중단 |
| WSS | 64바이트 | 5/5 | 통과 |
| WSS | 256바이트 | 5/5 | 통과 |

WSS 중앙값은 64바이트 `93,282 ops/s`, 256바이트 `92,128 ops/s`였다. pending queue와
dropped target은 모든 성공 cell에서 0이었다.

결과:
`bindings/c/perf/results/multi/report/perf_c_multi_linux_20260720_182726_s9-p02-router-fq-lock-final-secure.txt`

TLS 기능 자체와 100-peer 규모 문제를 구분하기 위해 같은 runtime에서 2-peer 대조 실행을
추가했다. TLS와 WSS의 64·256바이트 네 cell이 모두 통과했다. 따라서 현재 증거는 인증서
설정 전체의 회귀보다 100-peer TLS 시작 또는 admission 과정의 규모별 결함을 가리킨다.
정확한 실패 경계는 아직 확정하지 않았다.

대조 결과:
`bindings/c/perf/results/multi/report/perf_c_multi_linux_20260720_182802_s9-p02-router-fq-lock-secure-c2-control.txt`

## 판정

TCP의 기존 `_fq._pipes.empty()` 종료 회귀는 공식 Release runtime에서도 20회 반복을
통과했다. 그러나 100-peer TLS correctness가 실패하므로 S9-P02 correctness gate는 아직
완료가 아니다. 지시된 선행 조건에 따라 100 peer·5초·5회 Spot 대 ROUTER paired median은
실행하지 않았고 S9-P03의 기존 RED 판정도 유지한다.

이 라운드에서는 timeout, assertion, 공개 API, version과 package를 변경하지 않았다.
외부 배포와 bindings 내부 package 배포도 수행하지 않았다.
