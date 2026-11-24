# ZLink connector 예제

[English](README.md)

로컬 WebSocket 서버 하나에 브라우저, C#, Java 클라이언트를 연결합니다. 어느 클라이언트를
실행하든 서버를 시작하려면 Node.js 22 이상이 필요합니다.

## 서버 실행

`server/`에서 Linux는 `./run-server.sh`, Windows는 `run-server.cmd`를 실행합니다.
Windows에서는 PowerShell 7이 필요합니다. `READY ws://127.0.0.1:7301`이 표시되면 다른
터미널에서 클라이언트를 실행합니다. 서버를 끝내려면 Ctrl+C를 누릅니다. 서버는
`server/.logs/run-*/` 아래에 기록하는 로그 파일 경로를 출력합니다.

주소를 바꾸려면 서버를 시작하기 전에 `CONNECTOR_EXAMPLE_BIND`를 설정합니다. 서버가 출력한
주소를 브라우저의 입력란에 넣거나 터미널 클라이언트를 실행하기 전에
`CONNECTOR_EXAMPLE_ENDPOINT`에 설정합니다.

## 클라이언트 실행

| 클라이언트     | 디렉터리             | 명령                                                                                                                 |
| -------------- | -------------------- | -------------------------------------------------------------------------------------------------------------------- |
| 브라우저       | `client/typescript/` | `npm ci`와 `npm run browser`를 실행하고 [localhost:8000](http://127.0.0.1:8000/)에서 **Run**을 누릅니다.             |
| TypeScript E2E | `client/typescript/` | Node.js 22 이상에서 `npm ci`와 `npm run e2e`를 실행합니다.                                                           |
| C#             | `client/csharp/`     | .NET 8 SDK로 `dotnet run`을 실행합니다.                                                                              |
| Java           | `client/java/`       | JDK 25로 `./gradlew --max-workers=2 run`을 실행합니다. Windows에서는 `gradlew.bat --max-workers=2 run`을 사용합니다. |

Unity Editor와 네이티브 빌드에서는 NuGetForUnity로 버전이 고정된 `Zlink.Stream.Connector`를
설치하고 `ConnectorExample.cs`를 프로젝트에 복사합니다. Unity 스크립트에서
`ConnectorExample.RunAsync(endpoint, line => Debug.Log(line))`을 호출하고 완료를 기다립니다.
WebGL을 포함한 플랫폼별 설정은 [엔진 연동 가이드](https://docs.zlink.systems/dotnet/guide/stream-connector/12-engine-integration/)를
참조합니다.

## 메시지 흐름

1. 접속한 뒤 `send`로 단방향 메시지 `Publish`를 보냅니다. 서버는 별도 push로 `Notice`를 보냅니다.
2. `Reconnect`를 보내면 서버가 현재 세션의 연결을 끊습니다. Connector가 다시 연결됐다는 상태를
   알릴 때까지 기다립니다.
3. `Echo`를 요청합니다. 부호 있는 64비트 정수 `score`는 JSON 10진 문자열로 보냅니다. `nickname`은
   반드시 보내야 하며 값으로 `null`을 허용합니다. `label`은 생략하면 `"default"`를 사용하고,
   포함할 때는 문자열이어야 합니다.
4. `Leave`를 요청하고 `Left` 응답을 받은 뒤 연결을 닫습니다. `send` 직후 연결을 닫으면 서버의
   처리 완료를 보장할 수 없습니다.

브라우저는 `{"score":"9223372036854775807","nickname":null}`을 보냅니다. C#과 Java는 typed
codec이 언어의 `long` 값을 문자열로 인코딩합니다. TypeScript E2E는
`{"score":42,"nickname":null}`도 보냅니다. Connector는 `remoteError`와
`PayloadDecodeFailed: framework payload is not valid JSON.`을 반환합니다. JSON 문법은
유효하지만 서버의 typed decoder가 숫자로 전달된 `score`를 거절합니다. E2E는 `nickname`을
생략한 경우와 `label`에 `null`을 넣은 경우도 거절되는지 확인합니다.

[메시지 값 규칙](https://docs.zlink.systems/node/guide/server/20-channel-messaging/#12-message-value-rules)
