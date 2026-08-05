# Spot TLS 설정 전달과 72 cell correctness smoke

## Goal

MeshNode에 설정한 TLS server·client 자료가 실제 peer wire에 적용되는지 작은 회귀
시험으로 고정하고, Spot 3패턴의 72개 조합이 최신 Core runtime에서 모두 실행되는지
확인한다.

## 기준

- source commit: `57fa7ed956ce`
- 대상 변경 diff SHA-256:
  `a516fbb94b55764db1bcc092b3b5bd982c82c81a106eb9f64dee60efd5947c7b`
- runtime:
  `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.10.6.0`
- runtime 생성 시각: `2026-07-19 19:01:04 +0900`
- 실행 환경: WSL2, Linux

runner는 위 runtime을 사용한다고 출력했고, runtime은 이 라운드에서 수정한 Core
source보다 나중에 만들어졌다.

## Red gate

다음 명령은 TLS/WSS Spot req/rep의 작은 재현이다.

```bash
bindings/c/perf/run_benchmarks_multi.sh \
  --pattern SPOT_REQREP \
  --transports tls,wss \
  --msg-sizes 64 \
  --duration 1 \
  --clients 2 \
  --runs 1 \
  --results-tag s9-p01-correctness-red
```

수정 전에는 TLS가 `server_exit_before_ready_1`, WSS가 `non_zero_exit_1`로 종료되어
`success=0`, `fail=2`였다.

결과:
`bindings/c/perf/results/multi/report/perf_c_multi_linux_20260719_185254_s9-p01-correctness-red.txt`

Core integration test에서도 실제 test 인증서로 TLS MeshNode를 시작하면
`ZLINK_MESH_START_TLS_CONFIG_INVALID(705)`가 반환되는 red를 확인했다.

## 원인과 변경

`zlink_mesh_node_set_tls_server()`와 `zlink_mesh_node_set_tls_client()`는 입력을
검증했지만 자료를 MeshNode에 보존하지 않았다. peer wire가 내부 ROUTER를 만들 때도
이 설정을 전달하지 않아 TLS bind가 인증서 없이 실행됐다.

TLS 자료를 MeshNode 내부 상태에 보존하고, `wire_start()`가 bind 전에 공개
ROUTER TLS 설정 API로 적용하도록 수정했다. 공개 API나 인증 검증 조건은 바꾸지
않았다. integration test는 TLS와 WSS가 지원되는 build에서 실제 인증서 자료로
MeshNode bind가 성공하는지 검증한다.

## Green gate

TLS/WSS focused perf는 `success=2`, `fail=0`, `status=complete`였다.

결과:
`bindings/c/perf/results/multi/report/perf_c_multi_linux_20260719_190105_s9-p01-tls-green.txt`

이어 다음 smoke를 실행했다.

```bash
bindings/c/perf/run_benchmarks_multi.sh \
  --pattern SPOT_PUBSUB,SPOT_REQREP,SPOT_SENDSEND \
  --transports tcp,tls,ws,wss \
  --msg-sizes 64,256,1024,4096,65536,131072 \
  --duration 1 \
  --clients 2 \
  --runs 1 \
  --results-tag s9-p01-72cell-smoke
```

결과는 `success=72`, `unsupported=0`, `skip=0`, `fail=0`,
`status=complete`였다. 기존 조사 대상이던 WS 종료 assertion과 131072바이트
전환 종료도 이 조건에서는 발생하지 않았다.

결과:
`bindings/c/perf/results/multi/report/perf_c_multi_linux_20260719_190129_s9-p01-72cell-smoke.txt`

## 관찰과 다음 작업

이 실행은 correctness smoke이므로 정식 성능 판정에 사용하지 않는다. pub/sub과
일부 echo cell에서 mean이 p99보다 큰 결과가 있었다. 드문 긴 지연이 reservoir에
포함되지 않으면 가능한 값이다. 기존 공통 집계 helper는 이 정상적인 경우에도 p95를
mean 이상으로 강제로 바꾸고 있었다. 이 보정은 percentile의 뜻을 바꾸므로 제거했다.

고정 입력 test는 다음을 검증하며 `1/1` 통과했다.

- 1부터 100까지의 표본에서 mean `50.5`, p95 `95.05`, p99 `99.01`
- 0.1%의 큰 outlier가 있는 표본에서 mean이 p99보다 커도 값을 보정하지 않음
- 자식별 수신 건수로 가중한 reservoir 집계의 mean, p95, p99
- 고정 count와 duration의 초당 처리량, one-way와 echo의 bandwidth 방향 계수

```bash
cmake --build bindings/c/build \
  --target perf_multi_metrics_test \
           comp_src_spot_pubsub_client \
           comp_src_spot_reqrep_client \
           comp_src_spot_sendsend_client -j2
ctest --test-dir bindings/c/build \
  -R '^perf_multi_metrics_test$' --output-on-failure
```

같은 방향의 `MULTI_ROUTER_ROUTER_ONEWAY` 기준도 구현했다. hub와 각 peer는
각각 별도 process, context 1개, ROUTER 1개, I/O thread 1개를 사용한다. hub가
모든 peer에 같은 payload를 전송하고 각 peer가 수신 건수와 one-way latency를
보고하므로 Spot pub/sub와 같은 전송 방향과 자원 수로 비교할 수 있다.

2 peer에서 tcp, tls, ws, wss를 모두 실행한 결과는 `success=4`, `fail=0`,
`status=complete`였다.

결과:
`bindings/c/perf/results/multi/report/perf_c_multi_linux_20260719_192759_s9-p01-router-oneway-all-transport-smoke.txt`

10 peer tcp smoke도 `status=complete`였고 처리량은 `2.963656 Mmsg/s`였다.

결과:
`bindings/c/perf/results/multi/report/perf_c_multi_linux_20260719_192902_s9-p01-router-oneway-10peer-smoke.txt`

runner 정책 test는 21개가 모두 통과했고, metric test도 다시 통과했다.
따라서 S9-P01은 종료하고 100 peer·5초 paired profile로 S9-P02를 시작한다.
