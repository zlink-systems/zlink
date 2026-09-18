[English](./framework-dotnet-0.18.0.md) | [한국어](./framework-dotnet-0.18.0.ko.md)

# ZLink .NET Framework 0.18.0 릴리스 노트

Framework 0.18.0는 binding 1.2.0과 Core 1.2.0을 사용합니다. Framework 언어별 릴리스는 독립적으로 버전이 지정됩니다.

## 계약 변경

Stream connector의 공개 표면이 바뀝니다.

- 연결 이벤트 세 종이 C# `event`에서 `IDisposable`을 돌려주는 메서드가 됩니다. `+=`로 등록하던 자리는 메서드 호출로 바꾸고, 돌려받은 값을 해제할 때까지 보관해야 합니다.
- 오류는 닫힌 13개 코드로 통일됩니다. transport에 쓰기 전에 걸러지는 송신 한도 위반은 `ValidationFailed`입니다.
- Client connector에서 계기(metrics)를 걷어냈습니다.

## 공통 변경

- Runtime descriptor 변경의 게시 시점을 스펙에 적었습니다. 조율은 요청하는 쪽이 하고, 받는 쪽은 요청에 제약을 두지 않습니다. (#546)
- 원격 생성 예약 record의 `requestContentReference` 문법에서 checksum 구간을 없앰습니다. 예약은 별도의 record가 아니라 descriptor record의 상태입니다. (#559)
- 샘플은 한 번에 하나씩 실행합니다. 언어별 집계 러너를 없애고, 실행 방법을 공통 sample 문서가 소유하도록 했습니다. (#585)
- 언어별 e2e 시나리오 스위트를 걷어냈습니다. 크로스 언어 e2e는 유지합니다. (#541)

## 수정

- crash로 멈춘 ZoneNode를 같은 NodeId로 다시 시작할 때 이전 zone object를 되찾지 못해 ready에 이르지 못하던 것을 고쳤습니다. (#555)
- 노드 재시작 후 zone Spot 재합류 경로에서 취소가 그대로 빠져나와 프로세스가 중단되던 것을 고쳤습니다. (#542)
- runtime descriptor의 변경이 loop를 깨우지 않아 게시가 미뤄지던 것을 고쳤습니다. 네 setter가 하나의 통로를 지나도록 모았습니다. (#516)
- 샘플 teardown에서 강제 종료한 역할을 bash 실행이 실패로 만들지 않던 것을 고쳤습니다. 이전에는 강제 종료가 조용히 넘어갔습니다. (#575)
- 샘플 회귀 시험이 줄바꿈을 글자로 못 박아 CRLF 체크아웃에서 실패하던 것을 고쳤습니다. (#578)

## 설치

```bash
dotnet add package Zlink.Framework.AspNetCore --version 0.18.0
```

릴리스 태그는 [`framework-dotnet/v0.18.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-dotnet%2Fv0.18.0)입니다.
