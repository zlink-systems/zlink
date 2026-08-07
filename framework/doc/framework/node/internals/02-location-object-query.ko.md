# Node.js Object 위치 운영 조회

이 문서는 Node.js Framework의 public `listObjectLocations(...)`가 Location Store의 authority
record를 운영 조회 결과로 변환하는 내부 규칙을 정의한다. Application은 Store key, scan cursor,
authority payload를 직접 다루지 않는다.

## 조회 경계

조회 입력은 object kind를 필수로 받고 stable type과 MeshName을 선택적으로 받는다. Runtime은
authority scan 결과에서 owner lease가 현재 유효한 record만 남긴 뒤 public entry로 변환한다. 결과에는
global ID, object generation, MeshName, Node RID, 상태와 stable type만 포함한다.

`pageSize`는 public 계약의 `1..1000` 범위를 사용한다. Runtime은 한 번의 authority scan limit을
public page size와 같게 설정한다. authority scan을 항상 1000개 읽은 뒤 일부만 반환하면, 반환하지
않은 record가 다음 continuation token 뒤로 건너뛰므로 작은 page에서 누락이 발생한다.

## Instance Spot 구분

authority key의 기존 discriminator는 Actor와 Spot 계열을 구분하지만 User Spot과 Instance Spot을
서로 구분하지 않는다. 따라서 object query는 key discriminator를 object kind로 사용하지 않는다.
authority snapshot의 allocation kind를 기준으로 `user_spot`과 `instance_spot`을 구분한다.
이 규칙은 Instance Spot cold activation으로 만들어진 authority도 User Spot으로 잘못 보고하지 않게
한다.

## 수명과 오류

authority scan이 만료되면 부분 결과를 반환하지 않고 query를 실패시킨다. 호출자는 이전 page의
결과를 현재 전체 목록으로 간주하지 않아야 한다. owner lease가 만료된 authority는 운영 조회에서
제외하며, 현재 유효한 owner가 있는 record만 위치로 노출한다.

## continuation 중 변경

Continuation token은 호출자가 전달한 값을 그대로 사용한다. Node E2E의 SF-F6는 첫 page를
받은 뒤 public object request로 새 Instance Spot을 만들고 기존 Spot을 닫은 다음, 같은
continuation으로 조회를 끝까지 수행한다. 한 scan 안에서는 page size를 넘기지 않고 ID를
중복해서 반환하지 않아야 하며, 이후 새 scan은 변경된 object 집합을 반영해야 한다.

이 검증에서 object close는 테스트 전용 Store 조작이 아니다. Instance Spot handler가
Framework가 제공한 public context의 `close()`를 호출하는 application 동작을 사용한다.
따라서 query 구현은 authority record나 continuation 내부 형식을 E2E 호출부에 노출하지
않는다.

## Capacity 값의 해석

Actor와 Spot population limit에서 `0`은 제한 없음으로 해석한다. 이 의미는 placement
후보 선택, Location Store의 capacity 검사, relocation target 선택과 public MeshNode
descriptor에 동일하게 적용한다. activation concurrency limit은 별도의 admission
상한이며 population limit의 `0`을 대신하지 않는다.

capacity reservation이 상한을 넘으면 runtime은 일반 내부 예외로 바꾸지 않고
`CapacityExceeded`에 해당하는 Framework semantic error로 변환한다. 이 변환은
Instance Spot authority와 User Spot creation 경로에서 동일하게 적용되어야 하며,
E2E는 성공 개수만 세지 않고 실패 결과의 public error 의미와 rollback 뒤의 조회
상태까지 확인한다.

Instance Spot factory와 `onInitialize` 실행에는 별도의 activation concurrency gate가
적용된다. 이 gate는 동시에 실행 중인 factory 수만 제한하며, 성공한 Instance Spot의
population capacity와는 다른 값이다. 현재 실행 수는 public MeshNode descriptor의
`activationConcurrency.active`로 게시하고, gate 변화에 따른 descriptor write는 한
writer가 순서대로 처리한다.
