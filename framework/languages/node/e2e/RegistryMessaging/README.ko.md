# Node Config 1 Location Messaging E2E

이 디렉터리는 `framework/doc/framework/common/e2e/config-1-location-messaging.ko.md` 기준의
Node location messaging E2E 앱이다. 대응하는 `.NET` E2E는
`framework/languages/dotnet/e2e/LocationMessaging`이다.

검증용 HTTP endpoint는 실제 framework 역할을 실행하는 서버에 둔다. 별도 probe 서버가 상태 값을
만들어 내면 연결 상태와 무관하게 검증이 통과할 수 있기 때문이다.

- `Server/Provider`: profile provider, manual client, route mesh provider
- `Server/Workflow`: workflow channel provider
- `Server/Consumer`: client-only profile consumer와 public MeshNode runtime snapshot 기반 topology endpoint
- `Client`: scenario ID별 검증 실행

실행:

```bash
./run_e2e.sh
```
