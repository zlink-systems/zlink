[English](README.en.md) | 한국어

# ZLink 공개 스펙

이 디렉터리는 ZLink의 공개 API 계약을 정의한다. 대상 독자는 Core와 bindings 구현자, 공개 API
계약 검토자다. 함수 signature, 반환값, error, ownership과 thread safety는 이 목차에 연결된 정식 문서를
기준으로 판단한다.

## 1. 문서 구조

| 영역 | 문서 | 설명 |
|---|---|---|
| Core C ABI | [Core 스펙](core/README.ko.md) | C 함수, 타입, enum과 runtime 동작 계약 |
| Bindings | [Bindings 스펙](../../../bindings/doc/spec/README.ko.md) | Core 계약의 언어별 공개 API 투영 |

정식 spec은 현재 계약만 설명한다. 사용 목적과 예제는 guide가, 실제 내부 구조는 구현 완료 후
internals가 소유한다. 계약 검토자는 이 목차와 공개 header를 기준으로 탐색한다.

## 2. Core 주요 문서

| 문서 | 공개 계약 |
|---|---|
| [계약 관리](core/00-public-contract-governance.ko.md) | 정식 spec, header, test와 package의 일치 절차 |
| [Context](core/01-context.ko.md) | Context lifecycle과 option |
| [Message](core/02-message.ko.md) | message와 routing ID의 저장소·ownership |
| [Socket](core/socket/README.ko.md) | 범용 socket type과 send·receive 계약 |
| [Polling](core/06-polling.ko.md) | poll item, poller와 readiness |
| [Monitoring](core/07-monitoring.ko.md) | raw socket monitor와 snapshot |
| [Runtime 경계](core/09-runtime-boundary.ko.md) | Core raw C ABI와 Framework service 책임 경계 |
| [Events](core/05-events.ko.md) | 공개 event와 상태 전이 의미 |
| [Errors](core/03-errors.ko.md) | result enum, errno와 version ABI |
| [Errno map](core/04-errno-map.ko.md) | 함수별 result와 errno 대응 |
| [Utilities](core/08-utilities.ko.md) | timer, thread, stopwatch와 atomic utility |

## 3. 적합성

적합한 구현은 모든 정식 문서의 함수, 타입, 상수와 동작을 제공해야 한다. 공개 header, exported symbol,
contract test, bindings와 설치 package가 정식 spec과 다르면 적합하지 않다. 내부 구현 세부를 공개
계약으로 노출하거나 언어별 API가 공통 계약을 축소해서도 안 된다.
