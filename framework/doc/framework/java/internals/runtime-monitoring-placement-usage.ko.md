# Runtime monitoring placement usage

Store 조회 경계와 bounded fallback은
[runtime-monitoring-store-boundary](runtime-monitoring-store-boundary.ko.md)에 기록한다.

이 문서는 Java Framework의 public RouteMesh monitoring snapshot이 placement limit과 현재
사용량을 결합하는 경계를 설명한다. Placement의 공개 의미는
[공통 runtime monitoring spec](../../common/spec/server/24-runtime-monitoring.ko.md)과 Java monitoring
interface spec이 소유한다.

## 상태 source

Location Store descriptor는 node identity, placement weight, capacity limit과 capability를
제공한다. Descriptor의 `active` 값은 Framework process에서 object가 생성되거나 종료되는 순간의
현재 사용량을 보장하지 않으므로 public snapshot의 current count source로 사용하지 않는다.

Framework runtime은 object lifecycle의 실행 owner다. Snapshot을 만들 때 runtime은 descriptor가
제공한 limit·reserved 값과 runtime owner가 보유한 active Actor·User Spot count를 하나의 immutable
placement value로 합친다. 따라서 호출자는 Store row와 내부 registry를 따로 읽거나 두 결과를
조정할 필요가 없다.

## 일관성 규칙

- Actor와 User Spot의 active count는 snapshot 생성 시점에 runtime owner에서 읽는다.
- capacity limit, reserved count, object capability와 placement weight는 descriptor 또는
  registration에서 읽는다.
- Store를 일시적으로 읽을 수 없을 때도 registration에 있는 limit을 사용한다. 이 경우 topology
  state가 Store 장애를 별도로 나타내며 placement count source를 다른 private 경로로 바꾸지 않는다.
- snapshot은 새 `ZLinkPlacementSnapshot`으로 반환되며 이후 lifecycle 변경이 이미 반환한 값을
  변경하지 않는다.

## User Spot capacity admission

RouteMesh에서 User Spot을 만드는 경로는 public creation operation이 시작되기 전에 node의
configured spot limit을 확인한다. 이미 materialized된 User Spot과 아직 completion에 도달하지 않은
creation을 함께 세므로, 동시에 여러 creation이 제출되어도 limit을 초과한 backend object가 만들어지지
않는다. Creation이 실패하거나 terminal 상태에 도달하면 pending reservation을 해제한다.

이 admission은 monitoring snapshot의 count 계산과 별개의 lifecycle 책임이다. Snapshot은 현재 상태를
읽고, creation은 상태를 변경하기 전에 capacity를 예약한다. 두 경로가 Location Store descriptor의
초기 `active` 값을 각자 해석하지 않도록 runtime owner가 두 책임을 분리해 관리한다.

이 구조는 Location Store의 topology metadata와 Framework process의 live object state를 서로 다른
bounded context로 두면서, public monitoring 결과에서는 하나의 placement contract로 제공한다.

## Target readiness 확인

User Spot target 선택은 Store descriptor의 `SERVING` 상태만으로 완료되지 않는다. 선택을
진행하는 RouteMesh의 local status가 descriptor와 같은 lifecycle generation의 `READY`인지
확인하거나, remote target에 대한 peer entry가 같은 lifecycle generation으로 `ADMITTED`인지
확인해야 한다. 이 확인을 통과하지 못한 descriptor는 capacity 후보와 capacity 부족 판정에서
제외한다. 이렇게 해야 아직 peer admission이 끝나지 않은 target을 capacity 부족으로 잘못
보고하지 않고, readiness가 바뀐 뒤 같은 deadline 안에서 다시 조회할 수 있다.
