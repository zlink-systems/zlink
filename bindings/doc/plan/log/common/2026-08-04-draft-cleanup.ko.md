# Bindings 미승인 설계 문서 정리 기록 — 2026-08-04

> 이 log는 현재 공개 계약으로 승인되지 않은 bindings 설계 문서를 삭제하고, 삭제로 인해 계획 문서의
> 판단 근거가 끊기지 않도록 현재 계약과 남은 parity 작업을 기록한다.

## 결과

`bindings/doc/spec/draft/` 아래의 네 문서를 삭제했다. 네 문서 모두 구현 전 설계 후보이며 현재 공개 계약이
아니라는 문구를 포함하고 있었다. 이번 정리에서는 public header, 구현, 정식 spec, test와 package를 변경하지
않았다.

| 삭제한 문서 | 정리 판단 |
|-------------|-----------|
| `go-rust-submit-return.ko.md` | error-only submit 반환 제안은 승인되지 않았다. 현재 Go `Submit(ctx) (bool, error)`와 `false, nil` semantics를 유지한다. |
| `python-rust-single-part-naming.ko.md` | Python·Rust method 이름 차이는 아직 parity 결정이 아니다. 현재 각 언어 surface를 유지한다. |
| `route-mesh-python-go-rust.ko.md` | 이전 Core service header를 근거로 하므로 Core 0.9.0 raw-only bindings 입력으로 사용할 수 없다. 필요한 Framework 요구는 Framework 문서에서 별도로 확인한다. |
| `README.ko.md` | 삭제한 설계 후보 목록을 가리키는 index다. |

## 계획 문서 반영

- `bindings/doc/README.ko.md`에서 삭제된 draft index 링크를 제거했다.
- Go·Rust parity와 Python·Go·Rust 공통 계획에서 삭제된 문서 링크를 제거했다.
- 계획 문서에는 현재 공개 signature와 아직 끝나지 않은 parity 결정만 남겼다. 삭제는 설계 승인이나 API 변경을
  의미하지 않는다.
- `doc/site/mkdocs.yml`의 존재하지 않는 `bindings/spec/draft/` 제외 규칙을 제거했다. 별도의 루트
  `spec/draft/` 제외 규칙은 다른 문서 영역에 적용될 수 있으므로 유지했다.

## 검증 기준

- 삭제 대상은 일반 파일 네 개로 확인했으며 symbolic link는 없었다.
- 삭제 후 `bindings/doc/spec/draft/`에 파일이 남지 않아야 한다.
- 삭제된 파일을 가리키는 Markdown link가 `bindings/doc` 아래에 남지 않아야 한다.
- `git diff --check`가 통과해야 한다.

이 기록은 삭제된 설계 후보를 다시 public contract로 해석하지 않도록 하는 이력이다. 향후 contract 변경은
공통 spec governance에 따른 별도 review와 승인을 거친 뒤 정식 spec, 구현과 contract test를 함께 갱신한다.
