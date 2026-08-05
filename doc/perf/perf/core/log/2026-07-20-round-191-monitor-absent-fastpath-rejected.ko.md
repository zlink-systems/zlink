# Round 191: monitor 미설치 fast path 후보 반려

## 후보

Spot 송신·completion 경로는 monitor가 열리지 않은 일반 실행에서도 monitor event
함수에 진입해 MeshNode mutex를 획득한다. 두 대안을 비교했다.

1. 별도 atomic boolean hint를 두면 빠르게 건너뛸 수 있지만 monitor pointer와 상태
   지식을 중복한다.
2. 내부 monitor pointer 자체를 atomic으로 두면 pointer 하나를 상태 기준으로 유지하며,
   monitor가 없고 status counter도 갱신하지 않는 event만 lock 전에 종료할 수 있다.

두 번째 대안을 구현했다. monitor open·close, emitter pin, 실제 monitor queue와 status
counter 갱신은 기존 MeshNode mutex 경계를 유지했으며 공개 API는 바꾸지 않았다.

## correctness와 smoke

Debug focused 실행에서 MeshNode basic, monitor matrix, stress와 lifecycle 네 suite가
통과했다. peer admission 전체는 23개 중 기존 lifecycle generation 재시작 비교가 한 번
같은 값을 관측해 실패했고, 해당 `test_peer_drain_and_reconnect` 단독 재실행은 통과했다.
후보가 monitor queue 계약을 바꾸지 않았음은 focused monitor suite로 확인했지만, 아래
성능 smoke에서 효과가 없어 더 큰 correctness gate로 확장하지 않았다.

- focused log:
  `/home/hep7/.cache/zlink-core-validation/ctest-monitor-fastpath-candidate-focused.log`
- candidate runtime SHA-256:
  `f096d06674290a1532fc14b143163d416d25d608aa3ac16d5580d2054c6cb126`
- paired 결과:
  `bindings/c/perf/results/multi/paired/20260720-s9-p02-round191-monitor-absent-fastpath-c100/`

| 패턴 | Spot/ROUTER 처리량 비율 | mean 비율 | p95 비율 | p99 비율 |
|------|------------------------:|----------:|---------:|---------:|
| PUBSUB | 45.05% | 0.6595 | 1.5174 | 1.3303 |
| REQREP | 62.79% | 1.5239 | 2.5323 | 4.2084 |
| SENDSEND | 56.19% | 2.3117 | 2.4548 | 3.1296 |

세 처리량 비율이 모두 90%에 미달했고 기존 안정 중앙값 44.63%·64.24%·56.30%와
비교해 공통 개선이 없다. 지연 gate도 통과하지 못했다. 따라서 5회 paired median으로
확장하지 않고 후보를 반려했다.

## 원복

후보 전체를 원복하고 공식 `core/build`를 다시 만들었다. runtime SHA-256은 stable 값
`671fc61dcf4a462b599e6e2b315b1b1ec9765636351e209df3825fe792b33ffe`로 복원됐고,
`core/src`·`core/include`보다 오래된 runtime은 없다. version과 package는 변경하지 않았다.
