# .NET Config 1 Location Messaging E2E

이 디렉토리는 `framework/doc/framework/common/e2e/config-1-location-messaging.ko.md` 기준의
`.NET` location store 기반 messaging E2E 앱이다. registry process는 없다. 각 노드가
공식 Redis location store extension instance를 `AddLocationStore(...)`로 등록한다. 각 MeshNode는
descriptor를 자동으로 게시한다. store 상태는 `ListMeshNodesAsync(meshName)`, 연결 상태는
`IZLinkRouteMeshRuntime.Snapshot(meshName)`의 peer·channel snapshot으로 검증한다.

현재 source에 포함된 시나리오 ID는 아래와 같다. 10.0.0 계약에 맞춘 완료 여부는
`feature-map.ko.md`의 상태가 기준이다.

- `RM-A1` location store 자동 연결 + RID 자동 resolve — 10.0.0 전환 대상
- `RM-A2` 수동 endpoint 연결
- `RM-A4` 같은 RID, 다른 endpoint failover — 10.0.0 전환 대상
- `RM-A6` 서로 다른 MeshName 격리 — 10.0.0 전환 대상
- `RM-B1` scale-out — 10.0.0 전환 대상
- `RM-B2` scale-in / graceful drain — 10.0.0 전환 대상
- `RM-B3` owner lease 만료 뒤 failover — 10.0.0 전환 대상
- `RM-C1` request / send happy path
- `RM-C2` targeted request by rid
- `RM-C3` 다중 provider 분산
- `RM-C4` timeout과 late reply 비오염
- `RM-C5` 미등록 packet 처리
- `RM-C7` weighted 분산 — 10.0.0 전환 대상
- `RM-C8` payload 크기 변주
- `RM-C9` backpressure 관측 — 10.0.0 전환 대상

P1/P2 시나리오는 공통 문서의 지원 조건과 미배선 사유를 그대로 따른다.

실행:

```bash
./run_e2e.sh
```

Redis는 실행마다 전용 disposable `redis:7-alpine` container를 새로 시작한다. 이미 실행 중인
Redis나 host Redis endpoint는 재사용하지 않는다. 실행마다 run-unique key prefix
(`zlink:e2e:cfg1:<epoch>-<pid>`)로 격리하고, 종료 시 자신이 만든 container와 해당 prefix의
key를 정리한다.
