# Java RegistrationCodec E2E

이 E2E는 공통 Config 4 등록·codec 변주 시나리오를 Java framework public API로 검증한다.

역할은 `.NET` E2E와 같은 의미로 나뉜다.

| 위치 | 역할 |
|------|------|
| `Shared/` | channel 이름, packet DTO, evidence DTO를 공유한다. |
| `Client/` | HTTP client driver로 RC-A1~RC-B5 scenario 순서와 assertion을 실행한다. |
| `Server/Main/` | 등록 방식, DI lifecycle, filter ordering, JSON/Protobuf/MessagePack codec coexistence를 HTTP endpoint 뒤에서 제공한다. |
| `Server/InvalidDuplicate/` | duplicate packet registration startup failure를 검증하기 위한 실패 전용 role이다. |
| `Server/JsonOnlyPeer/` | RC-B5 codec mismatch에서 JSON만 등록한 peer를 제공한다. |
| `Server/CodecRequester/` | RC-B5 mismatch request와 recovery check용 HTTP endpoint를 제공한다. |

실행은 아래 명령을 사용한다.

```bash
timeout 420s ./run_e2e.sh
```

`run_e2e.sh`는 Gradle `installDist`를 실행한 뒤 role별 binary를 시작한다. Client는 framework runtime으로
뜨지 않고 `ZLinkHttpClient`로 server role을 구동한다. 실패하면 `logs/<run-id>/` 아래 stdout, stderr,
message flow log를 출력한다.

codec별 content-type은 public handler context인 `ZLinkHandlerContext.contentType()`으로 확인한다.
server handler가 request/send context의 content-type을 evidence로 남기고, client scenario가 JSON,
Protobuf, MessagePack 기대값을 검증한다. 이 검증은 raw frame이나 private runtime 접근을 사용하지
않는다.
