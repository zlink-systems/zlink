# Opaque Provider Authority의 Capacity Counter

이 문서는 Java Framework가 public opaque Location Store를 사용할 때 placement capacity를
보장하는 내부 구조를 설명한다. Application에는 이 자료구조나 key 형식을 노출하지 않는다.

## 책임 경계

Opaque provider는 Framework가 소유한 authority와 descriptor의 의미를 해석하지 않는다. 따라서
Framework의 semantic runtime core가 descriptor admission과 capacity 전이를 결정하고, provider에는
public `read`, `write`, scan 계약만 사용한다. Redis 전용 client나 provider의 private member를
Framework에서 호출하지 않는다.

## Capacity 상태

사용량은 host마다 하나인 capacity counter에 기록한다. Key와 value는
[Location runtime §3.4](../../common/spec/server/05-location-relocation/01-location-runtime.ko.md#34-여러-언어가-같은-redis-record를-읽고-쓰는-방법)가 정한다.

예약 요청은 다음 순서로 처리한다.

1. 동일 descriptor의 state, role, owner lease, capability와 capacity limit을 확인한다.
2. authority key와 capacity counter를 읽고 현재 `active + reserved + requested`가 각 limit 안에
   있는지 확인한다.
3. authority를 `PENDING`으로 만들고 capacity counter의 reserved 수를 증가시키는 mutation을 하나의
   조건부 provider write로 적용한다.

Capacity가 부족하면 `ZLinkPlacementCapacityExhausted`를 반환하며 authority를 만들지 않는다.
Descriptor가 없거나 lifecycle·owner가 일치하지 않으면 placement 후보가 사라진 것으로 처리한다.

## Lifecycle 전이

`commit`은 authority의 `PENDING`을 `ACTIVE`로 바꾸면서 capacity counter에서 reserved를 줄이고 active를
늘린다. `abort`와 `reject`는 authority를 삭제하면서 reserved를 줄인다. Active object를 close 또는
destroy하는 authority delete는 active를 줄인다. 각 전이는 authority version과 capacity counter
version을 함께 조건으로 사용하므로 두 기록 중 하나만 먼저 바뀌는 상태를 허용하지 않는다.

## Monitoring과의 관계

Runtime monitoring은 descriptor가 제공하는 limit과 현재 Framework runtime의 active Actor·Spot
수를 결합해 public placement projection을 만든다. `Placement.IsAvailable`은 Object Server,
positive placement weight, activation capacity, Actor capacity와 Spot capacity가 모두 남아 있을
때만 `true`다. Capacity counter는 예약의 원자성을 담당하고, monitoring projection은 public 상태
조회에 필요한 현재 count를 담당한다.

Capacity counter는 object create, commit, abort, close와 relocation 경로에서만 읽고 사용한다.
Message receive와 dispatch hot path에는 counter 조회, collection 복사, 추가 lock을 넣지 않는다.
