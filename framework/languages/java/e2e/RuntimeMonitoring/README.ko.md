# Java RuntimeMonitoring E2E

이 디렉터리는 공통 E2E Config 7 Runtime Monitoring 시나리오를 Java framework public API로 검증한다.
기존 Java `Monitoring` 구현은 `.NET` 기준 이름과 역할 구조에 맞춰 `RuntimeMonitoring`으로 정렬했다.

## 역할

- `Shared`: 공통 request, reply와 evidence 타입.
- `Server/Service`: channel, spot, socket/spot/location runtime monitoring source, evidence endpoint.
- `Server/FilteredService`: socket kind filter를 가진 별도 service role.
- `Server/ThrowingService`: monitoring handler failure를 격리하는 별도 service role.
- `Server/Trigger`: framework client와 HTTP scenario endpoint를 가진 trigger/validation process.
- `Client`: HTTP driver process. framework runtime으로 뜨지 않고 `Server/Trigger`의 scenario endpoint를 호출한다.

Runner는 실행별 role 설정 파일을 만들어 server에 전달한다. Client가 실행할 scenario는 시작할 때
검증하는 CLI 입력으로 전달한다.

## 실행

```bash
./run_e2e.sh
```

runner는 role별 Gradle `installDist` binary를 사용한다. 로그와 evidence는 `logs/<run-id>/` 아래에 남는다.
현재 완료/gap 분류는 `feature-map.ko.md`를 기준으로 본다.
