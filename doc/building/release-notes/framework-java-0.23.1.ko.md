[English](./framework-java-0.23.1.md) | [한국어](./framework-java-0.23.1.ko.md)

# ZLink Java·Kotlin Framework 0.23.1 릴리스 노트

Framework 0.23.1은 binding 1.4.0과 Core 1.4.0을 사용합니다. Framework 언어별 릴리스는 독립적으로 버전이 지정됩니다.

## 계약 변경

공개 API는 바뀌지 않습니다.

## 수정

- 수동으로 등록한 peer endpoint가 아직 연결되지 않은 상태에서 Location Store descriptor가 먼저 발견되면, descriptor 값으로의 교체가 계속 거부되어 두 node가 끝내 연결되지 않던 문제를 고쳤습니다. 연결된 적이 없는 연결 의도는 Core의 `disconnect`가 성공하면 닫힌 것으로 처리합니다. Server와 Client를 동시에 시작할 때 간헐적으로 `not_found`가 반복되던 증상이 이 문제였습니다. (#1034)
- Kotlin tutorial README의 실행·검증 블록이 Java 포트(5280)를 호출하던 것을 Kotlin 포트(5380)로 고쳤습니다. (#1035)
- ZoneWorld ZW-B8 fault proxy를 공용 `samples/Support/`로 옮겨 Java와 Kotlin 샘플이 같은 파일을 사용합니다. Kotlin 샘플 배포본에서 proxy를 찾지 못하던 문제를 고쳤습니다. (#1035)
- Kotlin ZoneWorld의 reporter 수명 테스트를 Kotlin으로 옮겨 suspend 함수 호출로 인한 컴파일 오류를 없앴습니다. (#1035)
