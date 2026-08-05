# Java ObservabilityOps E2E

이 디렉터리는 공통 Config 11의 OBS-A1~C5 증거를 검증한다. 검증기는 배포된 Java framework 역할이
생성한 scenario별 JSON을 읽는다. 테스트를 통과시키기 위한 임의 계기나 drain 결과를 만들지 않는다.

각 파일 이름은 `OBS-A1.json`부터 `OBS-C5.json`까지이며 `scenario` 필드가 파일 이름과 같아야 한다.
flow 배열은 `flow`, `origin`, `label`, `phase`, `outcome`, `sequence`를 사용한다. metrics 배열은 공통
스펙의 `name`, `kind`, `value`, `unit`, `tags` 형식을 사용한다. drain 증거는 `drainEvents`와
`peerRows`를 사용한다.

runner는 별도 endpoint와 Redis key prefix를 예약하고 이 config의 Delay, Play-A, Play-B, Session
역할 서버와 Client를 실행한다. 역할 서버가 함께 사용하는 topology 지원 코드는 한 모듈에 모아 두되,
각 역할의 실행 진입점과 OBS 시나리오 선택은 이 config가 소유한다. OBS-A1은 실제 actor relay 요청을
사용하고, OBS-A2는 등록되지 않은 packet을
보내 실제 error reply를 발생시킨다. OBS-A3은 Session의 tracing을 끈 뒤 같은 relay를 다시 실행하여
하류까지 flow가 유지되는지 확인한다. OBS-A4는 framework Spot timer에서 fanout event를 발행하고 두
Play 역할이 같은 flow를 수신하는지 확인한다. evidence JSON은 connector와 framework가 남긴 로그에서만
추출하며, 필수 로그가 없거나 flow가 다르면 실패한다.

Track B는 각 역할의 실제 Micrometer reader가 내보낸 snapshot을 사용한다. STREAM lifecycle과 reconnect,
사용자 Spot queue, 원격 actor transfer, fanout, owner lease 갱신 지연을 실제 사건으로 발생시킨다.
reader 미등록 검증은 metrics 자동 설정을 제외한 별도 Session 역할에서 8,192개 메시지를 처리하고,
ZLink meter series가 만들어지지 않았는지 확인한다. runner가 meter 예상 수치를 대신 만들지 않는다.

```bash
./run_e2e.sh all
```

Track A의 단일 시나리오는 OBS-A1부터 OBS-A4까지 지정할 수 있다.

```bash
./run_e2e.sh OBS-A2
```
