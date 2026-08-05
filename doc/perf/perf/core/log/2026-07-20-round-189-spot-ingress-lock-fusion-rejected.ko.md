# Round 189: Spot ingress lock 결합 후보 반려

## 계측 재판정

Round 170의 Callgrind raw 파일을 instruction 합계가 아니라 실제 호출 수로 다시
확인했다. C Spot req/rep server는 1,968개 request를 처리하는 동안
`pthread_mutex_lock`을 5,914회 호출했다. request 하나당 약 3.00회다.

- profile: `/home/hep7/.cache/zlink-core-validation/callgrind.spot-server.2470219`
- `handle_spot_data()` 호출: 1,968회
- `pthread_mutex_lock` 호출: 5,914회
- 이전 로그의 796,863은 lock 함수 안에서 실행한 instruction 수이며 호출 수가 아니다.

코드와 대조하면 peer·target 검증, request reply route 설치, mailbox admission이 각각
MeshNode mutex를 획득한다. 따라서 mutex 종류나 준비 알림을 다시 바꾸지 않고, 이 세
단계를 한 ingress transaction으로 묶는 후보를 검증했다. 대안은 다음 두 가지였다.

1. wire ingress가 mailbox 구현을 복제하면 lock 수는 줄지만 budget·transfer fence와
   rollback 지식이 두 모듈에 중복된다.
2. 기존 admission 모듈에 caller-held lock 경계를 두면 같은 정책을 재사용하면서
   검증·reply route·admission을 한 번의 lock 구간에 둘 수 있다.

두 번째 대안을 선택했다. multicast는 peer 검증과 subscription snapshot을 한 lock
구간으로 합쳤다. 공개 API와 framing, ownership 의미는 바꾸지 않았다.

## correctness와 paired 결과

별도 Debug build에서 MeshNode focused integration 5/5가 통과했다. 이어 공식
`core/build`를 candidate SHA-256
`bed35f6f7c086c97b88452bae4492c4cd6c283a331e7a1f5a1e5f6d50580a665`로 만들고
tcp·64바이트·100 peer·5초 paired 1회를 실행했다.

결과:
`bindings/c/perf/results/multi/paired/20260720-082411-s9-p02-round189-ingress-lock-fusion-c100/`

| 패턴 | Spot 처리량 | ROUTER 처리량 | 비율 | mean 비율 | p95 비율 | p99 비율 |
|------|------------:|----------------:|-----:|----------:|---------:|---------:|
| PUBSUB | 1,826,544.6 msg/s | 3,754,099.2 msg/s | 48.65% | 0.6285 | 0.5739 | 0.3951 |
| REQREP | 63,984.6 ops/s | 100,153.6 ops/s | 63.89% | 3.1389 | 2.5778 | 3.8993 |
| SENDSEND | 77,450.4 ops/s | 140,155.6 ops/s | 55.26% | 1.3594 | 2.2358 | 2.7925 |

세 처리량 비율이 모두 90%에 미달했다. REQREP와 SENDSEND 지연도 상한을 넘었으며,
P03 stable 중앙값 64.24%·56.30%와 비교해 공통 개선을 입증하지 못했다. mutex 획득
횟수는 확인된 비용이지만 현재 성능 차이의 주원인이 아니다.

## 판정과 원복

후보 전체를 원복했다. 공식 runtime을 다시 빌드한 뒤 SHA-256이 stable 값
`671fc61dcf4a462b599e6e2b315b1b1ec9765636351e209df3825fe792b33ffe`로 복원됐고,
`core/src`·`core/include`보다 오래된 runtime은 없다. version과 package는 변경하지
않았다.

다음 성능 후보는 lock 횟수만 줄이지 않는다. 성공한 메시지마다 실행되는 payload
frame 구성, ready claim과 receive view 이동, 즉시 재시도/yield의 실제 CPU 비중을
각각 측정한 뒤 선택한다.
