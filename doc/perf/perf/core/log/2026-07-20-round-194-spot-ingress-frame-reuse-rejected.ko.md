# Round 194: Spot ingress frame 저장소 재사용 후보 반려

## 후보

Mesh ingress thread는 wire message마다 새 `std::vector<zlink_msg_t>`를 만들었다.
일반 Spot message는 envelope와 payload frame으로 구성되므로 vector backing store가
성공 메시지마다 다시 할당되는지 확인했다.

두 대안을 비교했다.

1. custom small-vector를 추가하면 allocation을 없앨 수 있지만 별도 container 정책과
   예외·소유권 처리가 ingress 모듈 밖으로 확산된다.
2. ingress thread가 frame vector 하나를 소유하고 drain turn마다 재사용하면 기존
   vector와 message ownership을 유지하면서 backing store만 재사용한다.

두 번째 대안을 구현하고 capacity 4를 한 번 예약했다. `bad_alloc` 경로에서는 아직
소유한 frame을 닫도록 했다.

## correctness와 paired 결과

별도 Debug build에서 MeshNode basic 14/14와 stress 3/3이 통과했다. peer admission
전체는 24개 중 기존에 간헐적으로 같은 lifecycle generation을 비교하는
`test_peer_drain_and_reconnect`만 1회 실패했고, 단독 재실행은 통과했다. 다른 peer,
Spot, transfer와 shutdown 항목은 모두 통과했다.

공식 `core/build` candidate SHA-256
`7b452cfa5ceb868df564dc8ac83be8ecd4533a1b52ed8025f552abd538596a7f`로
tcp·64바이트·100 peer·5초 paired 1회를 실행했다.

결과:
`bindings/c/perf/results/multi/paired/20260720-092044-s9-p02-round194-ingress-frame-reuse-c100/`

| 패턴 | Spot 처리량 | ROUTER 처리량 | 비율 | mean 비율 | p95 비율 | p99 비율 |
|------|------------:|----------------:|-----:|----------:|---------:|---------:|
| PUBSUB | 1,835,523.4 msg/s | 4,151,668.8 msg/s | 44.21% | 1.6746 | 1.8103 | 1.2792 |
| REQREP | 56,973.2 ops/s | 89,904.0 ops/s | 63.37% | 3.3620 | 3.0114 | 5.1868 |
| SENDSEND | 69,158.4 ops/s | 110,843.8 ops/s | 62.39% | 1.4305 | 2.0644 | 2.5193 |

세 처리량 비율과 지연 gate가 모두 실패했다. 안정 5회 중앙값과 비교해 PUBSUB과
REQREP 절대 처리량이 낮고 SENDSEND만 단발 상승했다. frame vector backing store는
제거 가능한 allocation이지만 세 workload의 공통 병목이라는 증거가 없다.

## 판정과 원복

후보 전체를 원복했다. 공식 runtime을 다시 빌드한 뒤 SHA-256이 안정 값
`671fc61dcf4a462b599e6e2b315b1b1ec9765636351e209df3825fe792b33ffe`로 복원됐고,
`core/src`·`core/include`보다 오래된 runtime이 없다. version과 package는 변경하지
않았다.

다른 framework의 짧은 startup gate에 영향을 주지 않도록 고부하 perf 실행은
coordinator의 재개 신호 전까지 중단한다.
