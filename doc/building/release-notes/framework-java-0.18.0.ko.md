[English](./framework-java-0.18.0.md) | [한국어](./framework-java-0.18.0.ko.md)

# ZLink Java Framework 0.18.0 릴리스 노트

Framework 0.18.0는 binding 1.2.0과 Core 1.2.0을 사용합니다. Framework 언어별 릴리스는 독립적으로 버전이 지정됩니다.

## 계약 변경

Stream connector의 공개 표면이 바뀝니다.

- 표준 예외 대신 오류 코드를 담은 `ZLinkStreamException`을 던집니다. 표준 예외는 코드를 담을 자리가 없어 호출자가 `ValidationFailed`와 `ConfigurationError`를 구분하지 못했습니다.
- `ZLinkStreamConnectorOptions`는 구성 요소 21개의 record입니다. **자리 인자로 만들면 구성 요소가 늘 때마다 깨집니다.** `createDefault`에서 파생시키십시오.
- Kotlin 래퍼(`ZLinkKotlinStreamConnector`)에 종료 사유를 읽는 `closeReason()`이 생기고, `expectNone()`·`waitForSequence()`가 이름 없이 타입만으로도 이름을 정합니다. (#600)

## 공통 변경

- Runtime descriptor 변경의 게시 시점을 스펙에 적었습니다. 조율은 요청하는 쪽이 하고, 받는 쪽은 요청에 제약을 두지 않습니다. (#546)
- 원격 생성 예약 record의 `requestContentReference` 문법에서 checksum 구간을 없앰습니다. 예약은 별도의 record가 아니라 descriptor record의 상태입니다. (#559)
- 샘플은 한 번에 하나씩 실행합니다. 언어별 집계 러너를 없애고, 실행 방법을 공통 sample 문서가 소유하도록 했습니다. (#585)
- 언어별 e2e 시나리오 스위트를 걷어냈습니다. 크로스 언어 e2e는 유지합니다. (#541)

## 수정

- relocation 직후 gateway가 보낸 첫 actor message가 이미 닫힌 임시 큐에 넘겨져 배달된 것으로 보고되고 사라지던 것을 고쳤습니다. 임시 큐를 닫는 시점에 admission 자리표시도 함께 끝냅니다(스펙 08-routing §3). Windows에서 ZoneWorld가 `ZoneStateNotify`를 30초 기다리다 실패하던 원인입니다. (#632)
- 같은 노드 안의 `JoinSpot` 뒤 도착한 message가 Join 완료 콜백보다 먼저 dispatch되던 것을 고쳤습니다. dispatch 대상이 바뀌어도 deferred-Join barrier가 Actor 앞에 남습니다(스펙 05 §4). (#644)
- ActorClient의 message-follow 시험이 전체 실행에서 이따금 `one-way route is not connected`로 실패하던 것을 고쳤습니다. 송신 경로가 `VALIDATING_PREVIOUSLY_READY`를 허용하지 않아 생긴 경합입니다. (#533)
- 예약 record의 inline-v1 참조에 CRC32C 구간이 없어 다른 언어의 예약을 모두 거절하던 것을 고쳤습니다. (#559)
- 수신 큐 배출(`ZLinkStreamDispatchQueue.drainAsync`)이 항목마다 자기를 다시 불러 큐가 길어지면 `StackOverflowError`로 끝나던 것을 고쳤습니다. 이제 반복문으로 배출합니다. (#604)
- ZoneWorld에서 재기동한 node가 zone을 요구하는 cold start 구성으로 기동해 zone을 하나도 되찾지 못해 topology ready에 이르지 못하던 것을 고쳤습니다. 재기동은 zone 0개로 시작하는 replacement 구성을 씁니다. (#621)
- Windows PowerShell 5.1에서 `Start-Process`가 OS handle 없는 프로세스 객체를 돌려줘 `ExitCode`가 `$null`이 되고, 시나리오를 완주한 TicTacToe client가 실패로 판정되던 것을 고쳤습니다. Java·Kotlin 샘플 러너 전체가 공유 헬퍼 `Start-ZlinkSampleProcess`로 자식 프로세스를 띄웁니다. (#628)
- Windows에서 ZoneWorld `ZW-B8`이 PATH 밖의 Python 설치나 실행되지 않는 Store alias stub 때문에 fault proxy를 찾지 못해 멈추던 것을 고쳤습니다. `Get-ZlinkSamplePythonCommand`가 PATH·`py` 런처·표준 설치 위치를 차례로 살펴 실제로 동작하는 Python 3만 후보로 인정합니다. (#633)

## 설치

```kotlin
implementation("systems.zlink:zlink-framework-core:0.18.0")
```

릴리스 태그는 [`framework-java/v0.18.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-java%2Fv0.18.0)입니다.
