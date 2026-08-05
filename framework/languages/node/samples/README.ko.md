# ZLink Node.js Framework Samples

이 디렉터리에는 Node.js/NestJS framework의 11.1.0 공개 API를 사용하는 일곱 샘플이 있다. 각 샘플의
업무 흐름과 검증 기준은
[framework 공통 sample 문서](../../../doc/framework/common/sample/README.ko.md)를 따른다.

## 샘플 목록

| 샘플 | 보여 주는 기능 | client 실행 환경 | 연결 구성 |
|---|---|---|---|
| `Bingo.Ts` | Session, Entry Spot, room Spot, player Actor, timer와 bound-session push | Chromium | Redis location store |
| `TicTacToe.Ts` | API 2개와 Play 2개의 scale-out, room 조회와 실시간 게임 | Chromium | 수동 MeshNode peer와 Redis room route store |
| `SupportChat.Ts` | conversation owner, 상담 배정, reconnect, idle timer와 종료 알림 | Chromium | Redis location store |
| `DeliveryDispatch.Ts` | 배송 배차, timeout 재배정과 고객·기사 상태 알림 | Chromium | Redis location store |
| `GameQuest.Ts` | player별 quest owner Spot, event stream과 조회 모델 | Chromium | Redis location store |
| `ShoppingMall.Ts` | 주문 workflow, event stream, HTTP 조회 모델과 fanout event | Node.js HTTP client | Redis location store |
| `ZoneWorld` | Actor의 zone 이동, Logical Multicast, Node direct, runtime event와 관제 UI | headless Node.js scenario와 shared Chromium client | Redis location store |

TicTacToe만 MeshNode peer endpoint를 수동으로 설정한다. 다른 샘플은 Spot과 Actor 위치를 찾고 peer를
구성할 때 Redis location store를 사용한다.

## 문서 위치

업무 흐름, 메시지 계약과 smoke 순서는 공통 sample 문서가 소유한다. Node 샘플에 공통 내용과 다른
설정이나 실행 절차가 없으면 개별 sample 디렉터리에 README를 반복해서 두지 않는다. Node workspace
준비, Chromium 경계와 통합 실행 방법처럼 언어 전체에 적용되는 내용은 이 문서에서 한 번만 설명한다.
개별 차이가 생기면 해당 sample 디렉터리에 그 차이와 실행 방법만 추가한다.

## MeshNode와 channel

하나의 물리 mesh는 process마다 MeshNode 하나로 구성한다. ChannelName은 그 MeshNode가 참여하는
논리 service group이며 별도 ROUTER endpoint를 만들지 않는다. Node direct, ChannelName select-one,
Spot, Actor와 Logical Multicast는 같은 MeshNode를 사용한다. 모든 구독자에게 전달하는 classic
fanout은 별도 PUB/SUB channel이다.

## 브라우저 client 경계

Stream Connector를 사용하는 client는 브라우저용 ESM bundle로 만들어 실제 Chromium에서 실행한다.
Node.js는 bundle 생성, 정적 파일 제공과 headless Chromium 실행을 담당하며 Stream Connector의 client
runtime으로 사용되지 않는다.

ZoneWorld의 언어별 디렉터리에는 Node server와 headless 검증 시나리오를 둔다. 모든 언어 server에
연결하는 TypeScript 브라우저 UI는 `framework/languages/shared_sample/zoneworld/client/`에서 공유한다.
자세한 connector 사용법은
[TypeScript Stream Connector guide](../../../doc/framework/node/guide/stream-connector/README.ko.md)를 참고한다.

## 실행 준비

저장소의 Node framework workspace에서 dependency와 Chromium을 준비한다.

```bash
cd framework/languages/node
npm ci
npm run browser:install
```

`package.json`이 참조하는 bindings local package가 없다면 먼저
[local package 배포 안내](../../../../scripts/local-package/README.ko.md)에 따라 package를 만든다. 각
sample runner는 자신이 사용하는 Docker Redis container를 만들고 종료할 때 제거한다.

## 실행

샘플 하나를 실행하려면 Node framework workspace에서 해당 runner를 호출한다.

```bash
cd framework/languages/node
./samples/Bingo.Ts/run_sample.sh
```

지원하는 일곱 샘플을 순서대로 실행하려면 통합 runner를 사용한다.

```bash
./samples/run_samples.sh
```

예를 들어 Bingo의 전체 client 흐름은
[`bingo-client-scenario.ts`](Bingo.Ts/Client/bingo-client-scenario.ts)에서 확인할 수 있다.

일부 샘플만 실행할 때는 디렉터리 이름을 인자로 넘긴다.

```bash
./samples/run_samples.sh Bingo.Ts SupportChat.Ts ZoneWorld
```

Windows PowerShell에서도 같은 책임을 가진 runner를 사용한다.

```powershell
Set-Location framework/languages/node
./samples/Bingo.Ts/run_sample.ps1
./samples/run_samples.ps1
```

Runner는 역할별 설정 파일 생성, 서버 시작, readiness 확인, client self-check와 정리를 담당한다.
Framework host는 endpoint, Redis, routing ID, timeout과 로그 설정을 파일에서 typed configuration으로
읽는다. Application code가 이 값을 환경 변수에서 직접 읽지 않는다.

개별 runner는 성공하면 종료 코드 `0`과 `PASS <Sample>` marker를 출력한다. 실패하면 실패한 역할과
검증 항목을 표준 오류에 남기고 자신이 시작한 서버, Chromium과 Redis container를 정리한다.
