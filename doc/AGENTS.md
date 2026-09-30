# Documentation Guidelines

이 규칙은 `doc/` 아래 문서를 수정할 때 적용한다. 다른 문서 트리에서도 문서의 독자와 표현을
판단할 때 이 기준을 사용한다.

## 문서 역할

- `spec/`: 공개 API와 동작 계약. signature, 반환값, error, ownership과 순서를 정확히 적는다.
- `guide/`: application 개발자가 기능을 선택하고 사용하는 방법을 설명한다. 내부 배선은 넣지 않는다.
- `internals/`: 유지보수자를 위한 내부 구조, protocol, thread와 data flow를 설명한다.
- `plan/`: 구현 전 조사와 임시 작업 기록이다. 공개 계약이 아니며 공개 문서에서 링크하지 않는다.

Spec에 사용법을, guide에 내부 구현을, internals에 사용자 사용법을 섞지 않는다.

## 계약 작성 순서

- Framework public contract는 구현할 범위를 정식 spec과 exact language interface에 먼저 확정하고,
  같은 작업에서 구현과 contract test를 맞춘다. 구현하지 않을 추측성 범위를 미리 확장하지 않는다.
- 현재 구현과 목표가 다르면 formal spec에 구현 진행 상태를 기록하지 않고, 구현과 기능 test를
  확정된 계약에 맞춰 수렴한다.
- RouteMesh 11 Core raw-only 계약은 `core/doc/spec/core/`에 목표를 먼저 확정할 수 있다. 진행 이력은
  정식 spec에 넣지 않는다.
- 위 예외 밖의 미구현 Core·binding API는 `doc/spec/draft/`의 기능별 draft에 작성한다. 첫머리에
  구현 전 초안이며 현재 계약이 아님을 표시하고, 구현 후 정식 spec으로 옮긴다.
- 임시 plan의 항목 ID와 link를 공개 문서에 넣지 않는다.

공개 문서에서 plan link가 생기지 않았는지 필요한 범위에서 확인한다.

```bash
grep -rn "](.*plan/" --include='*.md' framework/doc/framework core/doc bindings/doc \
  | grep -v '/plan/'
```

## 문장과 용어

- 문장과 용어는 [`principal/documentation/documentation-principles.ko.md`](./principal/documentation/documentation-principles.ko.md)를
  따른다. 결론 먼저(7.10), 용어 도입(7.2·7.12), 명사 나열 풀기(7.4), 비유·의인화 금지(7.7), 검증 가능한
  서술(7.8), API 설명은 코드 예제의 줄 주석으로 옮기는 규칙이 그 문서에 있다.
- 공개 API와 내부 source comment는
  [`principal/source-comment-principles.ko.md`](./principal/source-comment-principles.ko.md)를 따른다.

## Diagram

- sequence와 flow는 Mermaid를 사용한다.
- memory layout과 stacked layer는 code fence 안의 ASCII diagram을 사용한다.
- ASCII diagram 안의 text는 한국어 문서에서도 English만 사용하고, 모든 행의 가시 폭을 맞춘다.
- ASCII diagram은 72자를 권장하고 80자를 넘지 않는다. Tab과 trailing space를 사용하지 않는다.
