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
