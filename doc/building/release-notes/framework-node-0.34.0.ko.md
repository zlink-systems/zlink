[English](./framework-node-0.34.0.md) | [한국어](./framework-node-0.34.0.ko.md)

# ZLink Node.js Framework 0.34.0 릴리스 노트

Framework 0.34.0은 Node.js binding 1.26.0과 Core 1.26.0을 사용합니다.

## 호환성 변경

- 0.34.0은 relocation 진행 상태를 authority record의 `aggregate.relocation` field에 기록합니다. 0.34.0으로 올리기 전에 진행 중인 relocation을 끝내고, 이 field가 남은 record가 있는 동안에는 0.33.x로 되돌리지 마십시오. 0.33.x는 이 field를 읽지 않습니다. (Location runtime §3.4, #4718, #4730)
- 0.34.0은 언어 간 aggregate relocation(SpotWide User Spot과 marker가 없는 Direct Join)을 지원하지 않습니다. 해당 relocation은 같은 언어의 노드 사이에서만 사용하십시오. 네 언어가 공유하는 §8.3 저장 형식으로의 전환은 0.35.0에 예정되어 있습니다. (#5354, #5422)

## 동작 변경

- 만료된 owner의 release wait를 authority record마다 따로 재지 않고 owner token마다 한 번 측정합니다. (#5302, #5349)
- relocation 진행 상태를 `aggregate.relocation`에 기록하고 수락된 요청의 완료 기록을 게시 전에 Relocation Store에 저장합니다. (#4718, #5040, #5085)
- Instance Spot one-way send를 target activation deadline 없이 배치합니다. 배치 후보가 없으면 Unavailable을 반환하기 전에 descriptor를 한 번 새로 읽습니다. (#5312, #5380, #5365)
- Actor Join은 같은 Actor의 이전 relocation 정리가 끝날 때까지 남은 deadline 안에서 기다립니다. (#5369, #5377, #5410)

## 결함 수정

- Entry Spot handler의 `context.outbound`가 Spot ID 주소로 `requestToSpot`·`sendToSpot`을 보낼 수 있습니다. 이전에는 항상 "Spot address outbound runtime is not started"로 실패했습니다. (#5425, #5436)
- 기한이 지난 mesh maintenance를 다음 timer 차례에 실행해 음수 timeout 경고를 없앴습니다. (#5357, #5363)
- 늦게 완료된 Yield가 다른 활성 turn을 해제하지 않습니다. (#5093, #5110)
- relocation target ingress는 Restore 시점부터 도착한 메시지를 dispatch 준비까지 보관합니다. (#5084, #5191)
- aggregate prepare가 capacity 갱신과 경합해도 participant marker를 유지합니다. User Spot이나 Actor 배치를 거절한 host 실행은 배치 후보에서 제외합니다. (#5312, #5345, #5296, #5297)
- 같은 RID의 peer가 재시작하면 RouteMesh가 admission 기대값을 갱신하고 admission 전 send는 Unavailable을 반환합니다. (#5320, #5347)
- 이전 relocation에서 capture한 request가 아직 전달되지 않았으면 같은 Actor turn에서 시작한 Join은 Unavailable을 반환합니다. 재전달된 send handler의 완료를 기다리지 않고 publication을 해제합니다. (#5369, #5382)
- Join commit 뒤 Actor activation 전에 도착한 메시지를 새 authority fence 때문에 버리지 않고 해당 Actor의 대기열에 보관합니다. (#5369, #5391)
- STREAM bind에서 Actor ID는 같지만 ObjectGeneration이 다르면 InvalidOperation을 반환합니다. route가 없거나 오래된 경우에는 Unavailable을 반환합니다. (#4774, #5003)
- relocation target은 target을 준비하고 소유권을 확정하기 전에 PREPARE coordinator fence를 현재 authority와 비교합니다. (#4762, #4779)
- Actor와 User Spot 생성 기한이 만료되면 DeadlineExceeded를 반환하며 호출자 취소와 구분합니다. (#5194, #5210)
- relocation 후 새 owner의 Spot route cache가 이전 owner를 반환하지 않습니다. (#5423, #5424)
- 원격 Spot send와 request가 수신한 metadata를 handler에 전달합니다. (#5224, #5239)
- direct request와 shutdown이 기한을 넘기면 완료되지 않은 마지막 단계를 진단 기록에 남깁니다. (#5316, #5419, #5332, #5418)
- 중복 bind callback은 Actor turn 안에서 읽은 현재 binding으로 결정합니다. (#5271, #5275)

## 성능 개선

- channel header를 service frame에 직접 쓰고 operation deadline에 min-heap을 사용합니다. (#4830, #5291)
- Framework의 비동기 실행 문맥을 하나의 copy-on-write carrier로 통합해 promise마다 전파하는 횟수를 줄였습니다. (#4705, #5017)

## 설치

```bash
npm install @zlink-systems/framework@0.34.0 @zlink-systems/http-client@0.34.0
```

릴리스 태그는 [`framework-node/v0.34.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-node%2Fv0.34.0)입니다.
