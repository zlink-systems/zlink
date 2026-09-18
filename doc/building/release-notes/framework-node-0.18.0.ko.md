[English](./framework-node-0.18.0.md) | [한국어](./framework-node-0.18.0.ko.md)

# ZLink Node.js Framework 0.18.0 릴리스 노트

Framework 0.18.0는 binding 1.2.0과 Core 1.2.0을 사용합니다. Framework 언어별 릴리스는 독립적으로 버전이 지정됩니다.

## 계약 변경

Stream connector의 공개 API는 바뀌지 않습니다. 관찰 가능한 변화가 하나 있습니다.

- 배출 중일 때 `dispatch()`가 그 완료를 기다리지 않고 돌아옵니다. handler 안에서 `dispatch()`를 부르던 코드의 교착이 풀립니다.

## 공통 변경

- Runtime descriptor 변경의 게시 시점을 스펙에 적었습니다. 조율은 요청하는 쪽이 하고, 받는 쪽은 요청에 제약을 두지 않습니다. (#546)
- 원격 생성 예약 record의 `requestContentReference` 문법에서 checksum 구간을 없앰습니다. 예약은 별도의 record가 아니라 descriptor record의 상태입니다. (#559)
- 샘플은 한 번에 하나씩 실행합니다. 언어별 집계 러너를 없애고, 실행 방법을 공통 sample 문서가 소유하도록 했습니다. (#585)
- 언어별 e2e 시나리오 스위트를 걷어냈습니다. 크로스 언어 e2e는 유지합니다. (#541)

## 수정

- ZoneWorld 샘플이 WSL에서 반복 실패하던 것을 고쳤습니다. monitor 이벤트 배출이 수신 loop 안에 있어, 그 loop가 대기에 들어가면 배출도 함께 멈췄습니다. 전용 loop로 분리했습니다. (#538)
- 계약 시험이 파일을 읽는 자리 145곳이 각각 줄바꿈 규칙을 가지고 있어 CRLF 체크아웃에서 어긋나던 것을 하나로 모았습니다. (#582)
- 게이트가 없어진 집계 러너를 실행하고 계약 시험이 그 경로를 단언하던 것을 개별 러너로 옮겼습니다. (#588)

## 설치

```bash
npm install @zlink-systems/framework@0.18.0
```

릴리스 태그는 [`framework-node/v0.18.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-node%2Fv0.18.0)입니다.
