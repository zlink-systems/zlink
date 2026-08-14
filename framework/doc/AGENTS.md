# Framework Documentation Guidelines

이 규칙은 `framework/doc/` 아래 문서에 적용한다. 문장과 diagram은 `doc/AGENTS.md`를 함께 따른다.

## 문서 위치

- 세 package 공통 공개 계약과 server 공개 계약: `framework/common/spec/`
- server runtime 공통 내부 설계: `framework/common/spec/40-internal-*.md`부터
  `52-internal-*.md`까지. 이 문서는 공개 규범 스펙이 아니며 같은 디렉터리에 있어도 공개 계약을
  추가하거나 변경하지 않는다.
- HTTP client 계약: `framework/common/spec/http-client/`
- Stream connector 계약: `framework/common/spec/stream-connector/`
- 언어별 exact public interface와 구현 차이: 각 package spec 아래 `languages/<lang>/`. 공통 의미와
  공통 내부 결정은 이 경로에 복사하지 않고, 언어별 표현이나 실제 구현 차이만 둔다.
- 언어별 사용 안내: `framework/<lang>/guide/{server,http-client,stream-connector}/`
- 특정 언어에만 해당하는 내부 구현 설명: `framework/<lang>/internals/`. 여러 언어가 공유하는 내부
  결정은 `framework/common/spec/40-internal-*.md`부터 `52-internal-*.md`까지가 소유한다.

새 문서를 `framework/languages/<lang>/doc/`에 추가하지 않는다. 계약은 package별로, 사용 안내는
언어별로 관리한다.

## 계약 변경

- 공통 동작과 언어별 표현은 `framework/common/spec/00-public-contract-governance.ko.md`를 따른다.
- 공통 목표 계약과 exact language interface를 함께 갱신한다. 현재 구현이 다른 언어는
  `99-implementation-gap`과 해당 interface의 차이 표에 기록한다.
- 다른 언어 구현이나 E2E만 보고 계약을 추가하지 않는다.
- `40-internal-*`부터 `52-internal-*`까지의 내부 설계 문서는 구현 완료 전의 목표 구조를 현재
  구조처럼 설명하지 않는다.
- 보호 경로는 루트 `AGENTS.md`의 명시적 승인 규칙을 그대로 적용한다.
