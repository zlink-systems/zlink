# Framework Documentation Guidelines

이 규칙은 `framework/doc/` 아래 문서에 적용한다. 문장과 diagram은 `doc/AGENTS.md`를 함께 따른다.

## 문서 위치

- 세 package 공통 계약과 server 계약: `framework/common/spec/`
- HTTP client 계약: `framework/common/spec/http-client/`
- Stream connector 계약: `framework/common/spec/stream-connector/`
- 언어별 exact public interface: 각 package spec 아래 `languages/<lang>/`
- 언어별 사용 안내: `framework/<lang>/guide/{server,http-client,stream-connector}/`
- 언어별 내부 설명: `framework/<lang>/internals/`

새 문서를 `framework/languages/<lang>/doc/`에 추가하지 않는다. 계약은 package별로, 사용 안내는
언어별로 관리한다.

## 계약 변경

- 공통 동작과 언어별 표현은 `framework/common/spec/00-public-contract-governance.ko.md`를 따른다.
- 공통 목표 계약과 exact language interface를 함께 갱신한다. 현재 구현이 다른 언어는
  `90-implementation-gap`과 해당 interface의 차이 표에 기록한다.
- 다른 언어 구현이나 E2E만 보고 계약을 추가하지 않는다.
- Internals는 구현 완료 전의 목표 구조를 현재 구조처럼 설명하지 않는다.
- 보호 경로는 루트 `AGENTS.md`의 명시적 승인 규칙을 그대로 적용한다.
