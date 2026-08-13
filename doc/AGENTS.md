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
- 현재 구현과 목표가 다르면 Framework의 `90-implementation-gap`에 사실만 기록한다.
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

- 결론과 조건을 먼저 설명하고, 처음 나오는 개념은 하는 일을 쉬운 문장으로 풀어 쓴다.
- 한국어 문서에서 `language-exchange`, `문서작성`과 영어 명사를 연속으로 붙인 압축 표현을 쓰지 않는다.
- actor, session, 연결과 상태를 사람이나 생물처럼 표현하지 않는다. `산다`, `살아 있다`, `붙는다`,
  `물려 있다`, `돈다` 대신 `존재한다`, `유지된다`, `연결한다`, `설정된다`, `동작한다`를 사용한다.
- `canonical`, `surface`, `shape`, `path` 같은 추상어보다 실제로 보장하는 동작을 적는다.
- API와 함수의 일대일 설명은 긴 산문 목록 대신 짧은 code example의 해당 호출 옆 주석에 둔다.
  예제의 핵심 계약은 코드 주석만 읽어도 알 수 있게 한다.
- 공개 API와 내부 source comment는
  [`principal/source-comment-principles.ko.md`](./principal/source-comment-principles.ko.md)를 따른다.

## Diagram

- sequence와 flow는 Mermaid를 사용한다.
- memory layout과 stacked layer는 code fence 안의 ASCII diagram을 사용한다.
- ASCII diagram 안의 text는 한국어 문서에서도 English만 사용하고, 모든 행의 가시 폭을 맞춘다.
- ASCII diagram은 72자를 권장하고 80자를 넘지 않는다. Tab과 trailing space를 사용하지 않는다.
