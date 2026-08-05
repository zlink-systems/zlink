# Round 200: 성능 도구 pattern 종류 캐시 후보 반려

## 원인 후보

Round 199의 REQREP server Callgrind에서 성능 도구의 `pattern_kind()`가 전체 instruction의
3.16%를 사용했다. 이 함수는 성공 메시지를 처리할 때마다 compile-time pattern 문자열로
새 `std::string`을 만들고 `REQREP`, `SENDSEND`를 차례로 검색했다. string 생성·해제와 allocator
비용도 같은 profile 상위 항목에 있었다.

이는 Core 비용이 아니라 Spot 성능 도구에만 들어간 반복 분류 비용이다. 대응 ROUTER 기준에는
같은 함수가 없으므로 작은 메시지 비교를 왜곡할 수 있다. server와 client 모두 같은 구현을
사용해 세 Spot 패턴에 공통으로 적용된다.

두 대안을 비교했다.

1. CMake target마다 pattern enum을 compile definition으로 추가하면 실행 비용은 없지만 target 선언과
   source가 같은 분류 정보를 중복 소유한다.
2. 함수 내부 static 값으로 첫 호출에 한 번만 문자열을 분류하면 기존 target 정의와 source 책임을
   유지하면서 반복 할당과 검색만 제거한다.

두 번째 대안을 후보로 선택했다. 두 perf source는 S3 Channel amendment iteration 3의 70-file
snapshot에 포함되지 않았다. 기존에 다른 작업이 추가한 blocking drain, weighted latency와 diagnostic
변경은 건드리지 않았다.

## correctness와 smoke

여섯 Spot perf binary build와 `perf_multi_metrics_test` 1/1이 통과했다. tcp 64바이트·2 peer·1초
기능 smoke도 세 pattern 모두 result를 출력하고 `success=3`, `fail=0`으로 끝났다. 다만 PUBSUB
diagnostic에는 pending application message 2개와 multicast dropped target 331,209개가 기록됐다.
짧은 2-peer publish 부하에서 이미 완전성 조건은 RED였으며, 100-peer paired 결과도 후보의 최종
반려 근거로 함께 보존했다.

c2 결과:
`bindings/c/perf/results/multi/report/perf_c_multi_linux_20260720_192210_s9-p02-pattern-kind-cache-c2-correctness.txt`

공식 `core/build` runtime SHA-256은 후보 전후 모두
`a57d91a90ae3c0d67ed7d469df848e70038ff700e93fd5989c889795755b3301`이었다.

## c100 paired 결과와 판정

결과:
`bindings/c/perf/results/multi/paired/20260720-192237-s9-p02-pattern-kind-cache-candidate/`

| 패턴 | Spot 처리량 | ROUTER 처리량 | 처리량 비율 | mean 비율 | p95 비율 | p99 비율 |
|------|------------:|----------------:|------------:|----------:|---------:|---------:|
| PUBSUB | 3,479,879.2 msg/s | 4,172,562.2 msg/s | 83.40% | 2.4371 | 3.1620 | 5.0479 |
| REQREP | 68,984.0 ops/s | 110,998.2 ops/s | 62.15% | 3.6682 | 2.4588 | 3.2932 |
| SENDSEND | 70,165.8 ops/s | 127,968.6 ops/s | 54.83% | 1.8228 | 2.6606 | 3.3018 |

세 처리량 비율은 모두 90% 하한에 미달했고 모든 패턴에 1.25배를 넘는 지연 항목이 남았다.
Round 197 중앙값보다 REQREP Spot 절대 처리량은 높았지만 대응 ROUTER 상승 폭이 더 커 비율은
64.01%에서 62.15%로 낮아졌다. PUBSUB 종료 snapshot에도 application message 199개와
12,736바이트가 남았다. REQREP와 SENDSEND의 pending queue, 세 패턴의 multicast drop은 0이었다.

반복 pattern 분류 비용은 실제였지만 현재 격차의 주원인이 아니며 처리량·지연·완전성 gate를
통과하지 못했다. 후보의 두 함수 hunk만 원복하고 여섯 perf binary도 원복 source로 다시 만들었다.
기존 perf source 변경과 Core source는 보존했다.

이번 라운드에서는 timeout, assertion, version과 package를 변경하지 않았다. 외부 배포와 bindings
내부 package 배포도 수행하지 않았다. S9-P02와 S9-P03은 계속 진행 중이다.
