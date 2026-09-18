# Java Stream Connector

Java STREAM client connector의 가이드다. 서버 애플리케이션과 시험 도구가 대상이다.

| 순서 | 문서 | 내용 |
|----|------|------|
| 1 | [Stream Connector 개요](01-overview.ko.md) | 무엇에 쓰고 어디서 도는가, 서버 framework와의 경계 |
| 2 | [설치와 첫 연결](02-getting-started.ko.md) | 패키지 설치, 최소 연결, 첫 송신과 수신 |
| 3 | [Connector 옵션](03-connector-options.ko.md) | 옵션 목록과 기본값, 값이 검증되는 시점 |
| 4 | [packet 송신](04-sending.ko.md) | send와 request, packet 이름이 정해지는 순서, codec |
| 5 | [packet 수신](05-receiving.ko.md) | 등록과 해제, dispatch mode, 수신 큐와 개수 |
| 6 | [연결 생명주기](06-lifecycle.ko.md) | 연결 상태, 재연결, heartbeat, 종료 사유 |
| 7 | [오류 처리](07-error-handling.ko.md) | 닫힌 오류 코드 집합과 언어별 전달 방식 |

파일 번호는 언어에 상관없이 같은 장을 가리키는 식별자다. 1~7장은 다섯 언어가 공유한다.

## 관련 문서

- 공개 계약: [Java 공개 계약](../../../common/spec/stream-connector/languages/java/03-stream-connector.ko.md)
- 서버 가이드: [Java 서버 가이드](../server/README.ko.md)
