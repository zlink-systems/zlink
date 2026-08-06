# ZoneWorld startup과 maintenance 시나리오의 .NET 구현 기준

이 문서는 .NET ZoneWorld sample의 실행 순서와 시나리오 검증 경계를 기록한다. 공통
ZoneWorld sample 계약을 변경하지 않으며, 다른 언어의 구현을 기준으로 사용하지 않는다.

## Startup readiness

두 ZoneNode는 서로의 mesh readiness를 기다리기 전에 동시에 시작한다. 한 node가 먼저
readiness를 기다리면 다른 node의 첫 actor·Spot 요청이 remote route가 준비되기 전에
실행될 수 있다. 따라서 runner는 두 process를 먼저 시작한 뒤 두 process의
`topology=ready` marker를 각각 확인한다.

ZoneNode bootstrap이 bot actor를 만들 때 public Framework 결과가 `Unavailable`이면
mesh route가 준비되는 동안 제한된 횟수로 다시 시도한다. 이 재시도는 bootstrap 경로에만
적용하며, timeout·cancellation·다른 error kind를 성공으로 바꾸지 않는다. bot 생성은
`GetOrCreate`를 사용하므로 같은 `ActorId`에 대한 반복 시도가 별도 actor를 만들지 않는다.

```text
Runner
  |
  +--> Start ZoneNode A
  +--> Start ZoneNode B
  |
  +--> Wait topology=ready for A and B
  +--> Start Gateway and scenario client
```

이 순서는 sample 실행의 readiness 조건이다. Framework public contract에 startup 시간
보장을 추가하지 않으며, runner timeout은 관찰을 중단하는 harness budget이다.

## Maintenance 시나리오의 owner 선택

Location Store가 zone owner를 실행마다 선택하므로 ZoneNode와 zone의 고정 배치를 가정하지
않는다. E1 시나리오는 Ops의 `WatchNodesRes`와 `SetMaintenanceRes`에 포함된 `NodeId`와
zone 목록을 사용해 source owner와 target owner의 관계를 확인한다.

target zone이 다른 node에 있으면 maintenance 중인 target Spot의 admission 결과가
`ZoneMaintenance`가 되는지와 원래 좌표가 유지되는지를 확인한다. target과 source가 같은
node이면 same-node zone 이동과 source node의 정상 동작을 확인한다. maintenance는
fanout으로 모든 node에 전달되지만, 비대상 node의 이동까지 중단시키는 broadcast stop으로
해석하지 않는다.

E2는 maintenance 중인 node에 이미 존재하는 player가 같은 node의 zone 안에서 계속
이동할 수 있는지 확인한다. 다른 node로의 arrival은 target Spot admission이 판정한다.
따라서 시나리오 코드는 `PlanWalkWithinZone`을 사용해 각 Move의 축별 최대 이동량을
지키며, zone 배치나 transport RID를 직접 선택하지 않는다.

## 실패 분류

시나리오가 timeout으로 끝나면 다음 순서로 분류한다.

1. 공통 sample 문서의 요구 동작과 실패한 checkpoint를 확인한다.
2. 실행 시점의 owner zone 목록과 public response를 evidence log에서 확인한다.
3. runner timeout인지, scenario가 고정 topology·잘못된 이동량·잘못된 lifecycle을
   가정했는지 확인한다.
4. 계약에 맞는 입력인데 요구 event가 발생하지 않은 경우에만 ZoneNode 또는 Framework
   구현 gap으로 분류한다.

이 분류를 거치지 않고 timeout 값을 늘리거나 scenario 전용 retry를 추가하지 않는다.
