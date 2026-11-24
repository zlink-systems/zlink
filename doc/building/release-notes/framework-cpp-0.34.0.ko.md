[English](./framework-cpp-0.34.0.md) | [한국어](./framework-cpp-0.34.0.ko.md)

# ZLink C++ Framework 0.34.0 릴리스 노트

Framework 0.34.0은 C++ binding 1.26.0과 Core 1.26.0을 사용합니다.

## 호환성 변경

- 0.34.0은 relocation 진행 상태를 authority record의 `aggregate.relocation` field에 기록합니다. 0.34.0으로 올리기 전에 진행 중인 relocation을 끝내고, 이 field가 남은 record가 있는 동안에는 0.33.x로 되돌리지 마십시오. 0.33.x는 이 field를 읽지 않습니다. (Location runtime §3.4, #4718, #4730)
- 0.34.0은 언어 간 aggregate relocation(SpotWide User Spot과 marker가 없는 Direct Join)을 지원하지 않습니다. 해당 relocation은 같은 언어의 노드 사이에서만 사용하십시오. 네 언어가 공유하는 §8.3 저장 형식으로의 전환은 0.35.0에 예정되어 있습니다. (#5354, #5422)

## 동작 변경

- 만료된 owner의 release wait를 authority record마다 따로 재지 않고 owner token마다 한 번 측정합니다. (#5302, #5349)
- relocation source capture와 target commit을 authority record의 `aggregate.relocation`에 기록합니다. (#4718, #5384)

## 결함 수정

- STREAM bind에서 Actor ID는 같지만 ObjectGeneration이 다르면 InvalidOperation을 반환합니다. route가 없거나 오래된 경우에는 Unavailable을 반환합니다. (#4774, #5003)
- relocation target은 target을 준비하고 소유권을 확정하기 전에 PREPARE coordinator fence를 현재 authority와 비교합니다. (#4762, #4779)
- MeshNode weight 변경 시 setter에서 descriptor를 게시하고, lease 갱신 시 게시를 다시 시도합니다. (#4932, #5339)
- STREAM close는 같은 binding에 먼저 제출된 reply와 push가 끝난 뒤 수행합니다. (#4889, #5134)
- 재개 wake가 거절되어도 Yield한 turn의 terminal 결과를 해당 gate turn에서 전달합니다. (#5093, #5186)

## 성능 개선

- 동기 typed MeshNode·Spot handler가 coroutine frame 없이 완료되며 typed JSON decode의 중간 token 순회도 제거했습니다. 요청 header와 deadline 문자열을 재사용합니다. (#4357, #5381, #5334, #5321)
- envelope header view를 decode할 때 빈 metadata map과 선택 문자열 복사에 따른 할당을 줄였습니다. (#5292, #5364)
- SpotWide Actor 대기열 소비 시 공유 gate 안에서 Actor lock을 다시 잡지 않습니다. (#5090, #5144)

## 설치

플랫폼에 맞는 Framework archive를 [GitHub Release](https://github.com/zlink-systems/zlink/releases/tag/framework-cpp%2Fv0.34.0)에서 선택하십시오.

릴리스 태그는 [`framework-cpp/v0.34.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-cpp%2Fv0.34.0)입니다.
