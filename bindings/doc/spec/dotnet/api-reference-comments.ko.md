# .NET API Reference 주석

이 문서는 .NET 바인딩 공개 API reference에 쓰이는 XML 문서 주석의
언어별 기준을 정의한다. 모든 언어에 공통으로 적용되는 원칙은
[`source-comment-principles.ko.md`](../../../../doc/principal/source-comment-principles.ko.md)를
따른다.

`bindings/dotnet/src/Zlink/Contracts/`의 XML 주석은 계약 문구다. 이 폴더의
public으로 보이는 모든 type과 member에는 XML 문서가 있어야 한다. 호출자가
API를 올바르게 쓰기 위해 알아야 하는 공개 동작을 설명해야 하며,
`core/include/zlink.h`와 바인딩 계약 문서와 맞아야 한다.

## .NET 적용 범위

`bindings/dotnet/src/Zlink/Contracts/` 아래 public 계약 멤버에는 XML 주석을
작성한다. runtime 구현 세부 사항은 public XML 주석이 아니라 runtime 주석이나
`doc/internals/`에 둔다.

메인 `Zlink.csproj`에서는 `CS1591`을 억제하지 않는다. XML 문서 경고를 켠
상태의 clean rebuild가 contract assembly의 누락 검증 기준이다. codec project는
별도 package이므로 자체 정책을 둘 수 있다.

## .NET 주석 형태

- XML 주석 본문은 영어로 작성한다.
- summary는 짧고 호출자 관점으로 작성한다.
- 단순 enum 값과 DTO field는 짧은 한 줄 summary로 충분하다.
- 한 줄 summary에 담기 어려운 계약 세부 사항만 `<remarks>`에 적는다.
- timeout, cancellation, callback, ownership, disposal, exception 계약은
  공통 소스 주석 원칙에 맞춰 `<remarks>` 또는 `<exception>`으로 명시한다.

## Guide와의 분리

API reference 주석은 튜토리얼이 아니다. 각 type 또는 member의 정확한 계약만
설명한다. 사용 패턴, 예제, 목적 설명은 `doc/guide/`에 둔다.

공개 멤버에 긴 배경 설명이 필요하면 XML 주석은 짧게 유지하고, guide 또는
바인딩 README에서 해당 guide 문서로 연결한다.

## 리뷰 체크리스트

공개 계약이 바뀌면 코드와 함께 XML 주석을 검토한다.

- `dotnet build bindings/dotnet/src/Zlink/Zlink.csproj --no-restore -t:Rebuild`
  실행 결과 XML 문서 경고가 0개인가?
- 공통 소스 주석 원칙의 공개 API 체크리스트를 만족하는가?
- 생성될 API reference 대상과 문구가 맞는가?
