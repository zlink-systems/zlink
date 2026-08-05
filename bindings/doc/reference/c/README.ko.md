한국어 | [English](README.en.md)

[C 바인딩 스펙](../../spec/c/README.ko.md)

# C bindings 레퍼런스

**이 binding은 자신만의 레퍼런스 tier가 없다 — 대신
[core의 레퍼런스 트리](../../../../core/doc/reference/README.ko.md)를 가리킨다.**

이 트리의 다른 모든 bindings 언어(dotnet/cpp/java/node/rust/python/go)는
`core/include/zlink.h`를 언어별 `Contracts`/`contracts` 레이어로 감싸며 자신만의 타입,
builder, 명명 규칙을 갖는다 — 각 언어 레퍼런스 트리가 문서화하는 건 바로 그 wrapper
레이어다. C binding은 이렇게 하지 않는다.
[C 바인딩 스펙](../../spec/c/README.ko.md#공개-계약-소스)에 따르면:

> In C, the native ABI itself is the binding contract. `bindings/c` does not add a second
> contract/runtime layer on top of the core C API.

`core/include/zlink.h`가 **곧** C binding의 public contract다, 그대로 — 같은 header를
core 자신의 18-category 레퍼런스 트리
([`core/doc/reference/`](../../../../core/doc/reference/README.ko.md))가 이미 함수 단위로
문서화하고 있다: `zlink_ctx_new`, `zlink_send_part`, `zlink_socket`, `zlink_poller_wait`,
그리고 export되는 다른 모든 심볼. 여기 `bindings/doc/reference/c/01-*.md`부터 `05-*.md`까지
두 번째 세트를 쓰면 그 트리의 내용을 다른 제목 아래 중복시키거나, 얇게 만들어 cross-reference
색인으로 만드는 것밖엔 안 된다 — 어느 쪽도 독자가 `core/doc/reference/`에서 직접 얻을 수 없는
정보를 더해주지 않는다.

## `bindings/c`에서 실제로 다른 부분

`bindings/c` 자체에 고유한, ABI와 구별되는 부분을 문서화하는 곳은 이 레퍼런스 트리가 아니라
C 바인딩 스펙이다:

- [Repository structure](../../spec/c/README.ko.md#저장소-구조) — `bindings/c/include/`,
  `bindings/c/tests/`, `bindings/c/samples/`, `bindings/c/perf/` 대 `core/include/`/`core/src/`.
- [Interface shape exceptions](../../spec/c/README.ko.md#인터페이스-형태-예외) — 이 트리의
  다른 언어가 따르는 상위 레벨 wrapper-binding 관례(fluent builder, `Received.Reply()`, result
  code 대신 exception)가 C엔 전혀 적용되지 않는 지점들.
- [Required feature coverage](../../spec/c/README.ko.md#필수-기능-커버리지)와
  [Actor and Spot route results](../../spec/c/README.ko.md#actor와-spot-route-결과) — 이
  binding의 test/sample에 특화된 리뷰 체크리스트이지, 추가 public API 표면이 아니다.

이들 중 어느 것도 이 트리의 다른 언어 category가 뜻하는 entry-unit 레퍼런스 자료가 아니다 —
이미 다른 곳에서 완전히 문서화된 ABI 위에 얹힌 packaging, testing, review-process 규칙이다.

## 로케일 관례

`bindings/doc/spec/<lang>/`의 모든 문서는 English 원본, Korean 번역이다, 이 파일과 동일하다.

---

실제 API 문서는 [C 바인딩 스펙](../../spec/c/README.ko.md)과
[core의 레퍼런스 트리](../../../../core/doc/reference/README.ko.md)에서 확인한다.
