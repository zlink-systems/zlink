# SupportChat Java sample porting inventory

## 기준

| 기준 | Java 대응 | 분류 | 상태 | 비고 |
|------|-----------|------|------|------|
| `.NET: Client` | `Client` | client | done | customer와 agent stream client를 띄우고 request 응답과 push payload를 self-check한다. |
| `.NET: Shared/Contracts` | `Shared/.../Messages.java` | shared-contract | done | 인증, 상담 생성, 참여, 메시지, typing, close, notification 계약을 record로 둔다. |
| `.NET: Server/Api` | `Server/Api` | server-role | done | token 검증과 conversation allocation orchestration을 API channel handler로 처리한다. |
| `.NET: Server/Session` | `Server/Session` | server-role | done | client-facing stream endpoint를 열고 session packet을 API/Support channel로 연결한다. |
| `.NET: Server/Support` | `Server/Support` | server-role | done | conversation domain state와 상담원 availability를 Support channel handler가 소유한다. |
| common: 공유 location store | `SampleLocationStore` + runner Redis | external-adapter | done | API, Session, Support가 같은 Redis endpoint와 key prefix를 사용하며 Session과 Support router는 이 store로 자동 연결한다. |
| common: client push wait interface | `SupportChatClientScenario` | validation | done | notification은 stream connector `waitFor(...).submit(...)` 경로로 기다리고, customer와 agent가 서로의 conversation push를 받는지 확인한다. |
| common: server evidence | `run_sample.sh` flow log and support assertion | validation | done | runner가 client marker, support assertion, message flow log를 확인한다. |
| common: Spot actor admission과 bound session push 구조 | `Server/Session`, `Server/Support` | architecture | done | Session은 인증 후 Support actor를 보장하고 stream session을 actor에 bind한다. Support는 `SupportEntrySpot`, `SupportUserActor`, actor request handler로 conversation packet을 처리하고 bound session push를 보낸다. |

## 남은 확인 사항

현재 Java `SupportChat` 샘플 inventory에는 남은 `gap` 또는 `partial` 항목이 없다. 이후 공통 샘플
문서나 release gate가 바뀌면 이 문서도 같은 기준으로 다시 대조한다.

## 검증

- `nice -n 15 timeout 600s ./run_sample.sh` 통과: `supportchat full client/server self-check completed`
