# Java ResilienceLifecycle E2E

이 디렉터리는 공통 E2E Config 5 Resilience/lifecycle 시나리오를 Java framework public API로 검증한다.
구조는 `.NET` 기준 구현의 책임 분리를 따른다.

## 역할

- `Shared`: client와 server가 함께 쓰는 request, reply와 evidence 타입.
- `Client`: HTTP driver process. framework runtime으로 뜨지 않고 provider/consumer process lifecycle을 제어한 뒤
  `Server/Consumer`의 scenario endpoint를 호출한다.
- `Server/Consumer`: Redis location store와 MeshNode ChannelName client를 가진 framework participant. scenario
  traffic을 보내고 public MeshNode runtime snapshot로 MeshNode descriptor를 조회한다.
- `Server/Provider`: provider process, framework handler, evidence/admin HTTP endpoint.

Runner는 실행별 role 설정 파일을 만들어 server에 전달한다. Client가 실행할 scenario는 시작할 때
검증하는 CLI 입력으로 전달한다.

## 실행

```bash
./run_e2e.sh
```

runner는 Gradle `installDist`를 먼저 실행한 뒤 Client suite를 시작한다. provider와 consumer
role binary의 시작, 종료, 재시작은 Client support가 담당한다. 실행 로그는 `logs/<run-id>/` 아래에 남는다.

현재 Java 구현의 완료 상태는 `feature-map.ko.md`를 기준으로 본다. `.NET Client/Scenarios/*.cs`에
대응하는 Java Client scenario 파일은 있으며, 구현된 scenario는 `Server/Consumer`의 HTTP endpoint를
호출한다.
