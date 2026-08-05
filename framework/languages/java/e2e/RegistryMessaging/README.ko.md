# Java RegistryMessaging E2E

이 config는 공통 E2E Config 1과 `.NET` `RegistryMessaging` 구현을 기준으로 Java framework의
location store 기반 messaging 흐름을 검증한다.

Java 실행은 provider, workflow, consumer, client를 별도 Gradle application과 별도 process로 시작한다.
각 scenario는 Redis location store extension을 공유하고 실행마다 key prefix를 분리한다.
client는 framework를 직접 호출하지 않고, `.NET` 기준처럼 역할 server의 HTTP endpoint를 호출한다.
Consumer role은 location store 자동 연결, direct, single-provider, backpressure 모드로 나뉘어
실행되며, 각 모드는 public Spring starter와 public framework API만 사용한다.

동적 provider lifecycle을 검증하는 `RM-A4`, `RM-B1`, `RM-B2`는 client support가 provider와
consumer process를 직접 시작하고 종료한다. scenario는 provider를 requester로 쓰지 않고,
consumer가 location store row를 보고 자동 연결한 뒤 traffic을 보내는지 확인한다.

실행:

```bash
./run_e2e.sh
```

단일 scenario는 이름을 넘겨 실행한다.

```bash
./run_e2e.sh RM-A1
```

성공하면 각 client 단계가 `registry-messaging e2e result=passed`를 출력한다.

참고: `RM-C9`는 one-way send가 public 완료 객체나 bounded-failure oracle을 노출하지 않는다는
공통 계약을 따른다. Java scenario는 public API만 사용해 다량 send 제출과 recovery를 검증하고,
직접적인 HWM 오류 결과 검증은 binding/runtime 내부 테스트 범위로 둔다.
