# Round 197: TLS 인증서 파일 생성 경쟁 수정과 P03 paired RED

## 범위

- Core runtime: `core/build/lib/libzlink.so.10.6.0`
- runtime SHA-256:
  `a57d91a90ae3c0d67ed7d469df848e70038ff700e93fd5989c889795755b3301`
- 실행 환경: WSL2, Linux, Release
- correctness 대상: `MULTI_SPOT_REQREP`, 64·256바이트, 100 peer, active 1초
- paired 대상: Spot 세 패턴, tcp 64바이트, 100 peer, active 5초, 패턴별 5회

Core source와 정식 spec은 이 라운드에서 바꾸지 않았다. runner는 모든 실행에서 위 runtime
경로를 출력했고, `core/src`와 `core/include`에는 runtime보다 새로운 파일이 없었다.

## TLS RED 원인

Round 196에서는 같은 runtime의 2-peer TLS가 통과했지만 100-peer TLS는 첫 64바이트
단계에서 5회 모두 client 종료 코드 1로 실패했다. 프로세스 생성만 추적한 실행에서는
100개 자식 중 19개가 active phase와 admission 대기 전의 MeshNode 구성 실패 코드 11로
종료했다.

파일 열기를 추적하자 성능 도구의 server certificate, private key와 CA 파일을 server와
100개 client 자식이 각각 같은 `/tmp/bench_*.pem` 경로에 기록했다. 세 파일은 실행마다
각각 101회 `O_TRUNC`로 열렸고, 한 자식이 인증서 파일을 읽는 동안 다른 자식이 같은
파일을 truncate하는 순서가 실제 trace에 기록됐다.

따라서 실패 원인은 Core TLS나 peer admission이 아니라 C 성능 도구의 인증서 파일 생성
경쟁이다. MeshNode spec은 TLS 자료를 `start` 전에 설정하도록 요구하며, Core가 기록 중인
불완전한 PEM 자료를 거부한 것은 계약에 맞다. timeout을 늘리거나 Core의 인증서 검증을
완화할 이유가 없다.

RED 증거:

- `/home/hep7/.cache/zlink-tls-c100-process.strace`
- `/home/hep7/.cache/zlink-tls-c100-cert-open.strace`
- `/home/hep7/.cache/zlink-tls-c100-cert-open-red.log`

## 수정

`bindings/c/perf/common/perf_tls_setup.hpp`의 POSIX 인증서 파일 생성을 바꿨다. 각 프로세스는
`mkstemp()`로 별도 임시 파일을 만들고 전체 내용을 기록한 뒤 파일을 닫는다. 기록과 close가
모두 성공한 경우에만 고정 경로로 원자적으로 rename한다. 실패한 임시 파일은 제거한다.

이 방식은 reader가 이전의 완전한 파일이나 새로 교체된 완전한 파일만 열게 한다. fork를
사용하지 않는 Windows 경로는 바꾸지 않았다. 공개 API, Core source와 admission 동작은
변경하지 않았다.

수정 뒤 같은 파일 경로를 추적한 100-peer TLS 실행에서는 최종 경로의 `O_TRUNC`가 세 파일
모두 0회였고 실행도 통과했다.

GREEN trace:

- `/home/hep7/.cache/zlink-tls-c100-cert-publish-green.strace`
- `/home/hep7/.cache/zlink-tls-c100-cert-publish-green.log`

## Correctness 결과

| 전송 | 반복 | 64바이트 중앙값 | 256바이트 중앙값 | 결과 |
|------|------|-----------------:|------------------:|------|
| tcp | 20회 | 121,087.0 ops/s | 117,300.5 ops/s | 통과 |
| TLS | 5회 | 107,392.0 ops/s | 109,529.0 ops/s | 통과 |
| WSS | 5회 | 101,984.0 ops/s | 102,083.0 ops/s | 통과 |

여섯 cell의 pending application message, pending infrastructure message, pending byte와
dropped target은 모두 0이었다. assertion, timeout과 비정상 자식 종료도 없었다.

결과:

- `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260720_183938_s9-p02-tcp-c100-atomic-cert-regression.txt`
- `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260720_183838_s9-p02-tls-c100-atomic-cert-green.txt`
- `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260720_183918_s9-p02-wss-c100-atomic-cert-regression.txt`

`bindings/c/build` 전체 빌드와 `perf_multi_metrics_test`도 통과했다.

## P03 paired 결과

correctness 선행 조건이 충족된 뒤 tcp 64바이트·100 peer·5초 조건에서 Spot과 대응
ROUTER를 교차 순서로 각각 5회 실행했다. source tree와 runtime SHA-256은 시작부터 종료까지
변하지 않았고 multicast dropped target은 0이었다.

| 패턴 | Spot 중앙값 | ROUTER 중앙값 | 처리량 비율 | mean 비율 | p95 비율 | p99 비율 |
|------|------------:|----------------:|------------:|----------:|---------:|---------:|
| PUBSUB | 3,343,220.0 msg/s | 4,208,472.8 msg/s | 79.44% | 1.8936 | 3.8601 | 5.1811 |
| REQREP | 60,478.2 ops/s | 94,488.4 ops/s | 64.01% | 3.5544 | 2.5069 | 3.2258 |
| SENDSEND | 62,298.2 ops/s | 119,386.6 ops/s | 52.18% | 1.7807 | 2.8338 | 3.6576 |

세 패턴 모두 처리량 90% 하한과 mean·p95·p99 1.25배 상한을 통과하지 못했다. 따라서 TLS
correctness blocker는 닫지만 S9-P03은 RED를 유지한다.

paired 결과:
`bindings/c/perf/results/multi/paired/20260720-184210-s9-p03-router-fq-lock-final/paired-gate.md`

이 라운드에서는 timeout, assertion, version과 package를 변경하지 않았다. 외부 배포와
bindings 내부 package 배포도 수행하지 않았다.
