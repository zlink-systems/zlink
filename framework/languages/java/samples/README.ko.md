# ZLink Java/Kotlin Samples

Java·Kotlin 공통 sample 시나리오 일곱 개([공통 sample 문서](../../../doc/framework/common/sample/README.ko.md)가
정의)를 담은 디렉터리다. Java sample은 `java/`, Kotlin sample은 `kotlin/` 아래에 있다.

English: [`README.md`](./README.md)

## 전제 조건

- **JDK 25.** Gradle toolchain이 25로 고정돼 있다(`gradle/zlink-jvm-baseline.settings.gradle.kts`).
  자동으로 JDK를 내려받는 toolchain resolver(예:
  `org.gradle.toolchains.foojay-resolver-convention`)는 없다 — 실행 script들이 스스로
  기존 JDK 25를 찾는다(`JAVA_HOME`, `PATH`, `~/.gradle/jdks`와 그 밖의 흔한 설치 위치. 자세한
  탐색 순서는 `gradle/zlink-jvm-runtime.sh`·`redis-common.ps1`의
  `Set-ZlinkSampleJavaRuntime` 참고). 없으면 [Temurin 25](https://adoptium.net/)를 설치하고
  `JAVA_HOME`을 그 경로로 둔다.
- **Docker Desktop.** 각 sample 실행 script가 자기 몫의 Redis 컨테이너를 직접 띄우고
  지운다(`redis-common.ps1` / `runner-common.sh`) — Docker가 먼저 떠 있어야 한다.

Python은 필요 없다. Linux 포트 예약 헬퍼(`runner-common.sh`의
`zlink_sample_reserve_ports_in_range`)와 Windows ZoneWorld ZW-B8 fault proxy
(`ZoneWorld/Support/SessionRouteBlockProxy.java`) 모두 JDK single-file source
program(`java <file>.java ...`)으로 돌아 위 JDK 25만 있으면 된다.

## 내려받기와 설치

저장소를 checkout하지 않는다. 이 zip을 푼 디렉터리(`zlink-samples-java/`)가 전부고, 배포된
`zlink-framework-*` 패키지를 Maven Central에서 받아 빌드한다(버전은
`gradle/zlink-sample-dependencies.settings.gradle.kts`의 `zlink.frameworkVersion` 기본값
참고). 별도로 내려받거나 설치할 것은 없다 — Gradle wrapper가 Gradle을, Gradle이 위 패키지를
받는다.

```bash title="linux"
cd zlink-samples-java
```

```powershell title="windows"
Set-Location zlink-samples-java
```

## 빌드

실행 script(아래 "실행")가 빌드까지 함께 하므로 따로 빌드할 필요는 없다. IDE 연동이나
CI에서 실행 없이 빌드만 확인하려면, 이 zip을 푼 디렉터리(`zlink-samples-java/`) 안에서
다음을 쓴다.

```bash title="linux"
./gradlew projects
./gradlew buildAllSamples
```

```powershell title="windows"
.\gradlew.bat projects
.\gradlew.bat buildAllSamples
```

## 실행

Sample 하나마다 `run_sample.sh`와 `run_sample.ps1`이 있고, 한 번 실행하면 sample 하나를
끝까지 돌린다. Redis는 스크립트가 자기 몫의 컨테이너를 직접 띄우고 끝나면 지우므로 따로
띄울 필요가 없다(위 "전제 조건"의 Docker만 있으면 된다). 아래 명령도 모두
`zlink-samples-java/` 안에서 실행한다.

Linux·WSL:

```bash title="linux"
./java/Bingo/run_sample.sh
./java/DeliveryDispatch/run_sample.sh
./java/GameQuest/run_sample.sh
./java/ShoppingMall/run_sample.sh
./java/SupportChat/run_sample.sh
./java/TicTacToe/run_sample.sh
./java/ZoneWorld/run_sample.sh
./kotlin/Bingo/run_sample.sh
./kotlin/DeliveryDispatch/run_sample.sh
./kotlin/GameQuest/run_sample.sh
./kotlin/ShoppingMall/run_sample.sh
./kotlin/SupportChat/run_sample.sh
./kotlin/TicTacToe/run_sample.sh
./kotlin/ZoneWorld/run_sample.sh
```

Windows:

```powershell title="windows"
pwsh -NoProfile -ExecutionPolicy Bypass -File .\java\Bingo\run_sample.ps1
pwsh -NoProfile -ExecutionPolicy Bypass -File .\java\DeliveryDispatch\run_sample.ps1
pwsh -NoProfile -ExecutionPolicy Bypass -File .\java\GameQuest\run_sample.ps1
pwsh -NoProfile -ExecutionPolicy Bypass -File .\java\ShoppingMall\run_sample.ps1
pwsh -NoProfile -ExecutionPolicy Bypass -File .\java\SupportChat\run_sample.ps1
pwsh -NoProfile -ExecutionPolicy Bypass -File .\java\TicTacToe\run_sample.ps1
pwsh -NoProfile -ExecutionPolicy Bypass -File .\java\ZoneWorld\run_sample.ps1
pwsh -NoProfile -ExecutionPolicy Bypass -File .\kotlin\Bingo\run_sample.ps1
pwsh -NoProfile -ExecutionPolicy Bypass -File .\kotlin\DeliveryDispatch\run_sample.ps1
pwsh -NoProfile -ExecutionPolicy Bypass -File .\kotlin\GameQuest\run_sample.ps1
pwsh -NoProfile -ExecutionPolicy Bypass -File .\kotlin\ShoppingMall\run_sample.ps1
pwsh -NoProfile -ExecutionPolicy Bypass -File .\kotlin\SupportChat\run_sample.ps1
pwsh -NoProfile -ExecutionPolicy Bypass -File .\kotlin\TicTacToe\run_sample.ps1
pwsh -NoProfile -ExecutionPolicy Bypass -File .\kotlin\ZoneWorld\run_sample.ps1
```

## 검증

각 runner는 역할별 process를 띄우고, 준비를 기다리고, probe·client 시나리오를 돌린 뒤 자기가
만든 process와 Redis 컨테이너를 지운다. `ZLINK_FRAMEWORK_READY`에 도달한 Framework
process는 `ZLINK_FRAMEWORK_TERMINATION outcome=STOPPED reason=NONE`도 반드시 남겨야
한다 — 이 마커가 없거나, `STOPPED`/`NONE`이 아니거나, 강제 종료됐거나, 정리가 실패하면 그
sample은 실패다. 종료 코드 `0`과 "Redis 컨테이너/역할 process 정리 완료" 외에 별다른
출력이 없는 것이 정상 종료다.

## 문제 해결

- **`No matching toolchain found`.** JDK 25가 없다. "전제 조건"의 Temurin 25 설치 절차를
  따른다.
- **`Connection refused` (Docker).** Docker Desktop이 떠 있지 않다. 띄운 뒤 다시 실행한다.
- **`No bindable ... Redis host port remained` 또는 sample 포트 충돌.** 같은 언어의 sample을
  동시에 여러 개 돌렸거나, 이전 실행이 정리되지 않고 남아 있다. 각 언어는 자기 포트
  범위(`gradle/zlink-jvm-runtime.sh`의 `zlink_sample_configure_port_pool` 참고)를 쓰므로
  같은 언어를 동시에 두 개 이상 돌리지 않는다.
- **`UnsupportedClassVersionError`.** 실행 시점의 `JAVA_HOME`이 JDK 25보다 낮다. runner가
  스스로 JDK 25를 찾아 `JAVA_HOME`을 맞추지만(위 "전제 조건"), 그 탐색이 실패하면 JDK 25를
  설치하고 `JAVA_HOME`을 그 경로로 둔다.
- **압축 해제 경로가 깊어도 `입력 파일이 너무 깁니다`는 나지 않는다.** `installDist`가 만든
  실행 script의 classpath가 jar를 하나씩 나열하지 않고 `lib` 디렉터리 wildcard를 쓰기
  때문이다(Windows cmd.exe의 8191자 한도와 무관하다).

## Samples

| Sample | Main framework behavior | Peer topology |
|---|---|---|
| `Bingo` | Session gateway, Entry and room Spots, Actor binding, timers, and bound-session push | Redis location store |
| `TicTacToe` | Two API roles, two Play roles, room lookup, Actor turns, and real-time messages | Manual MeshNode peers; Redis room route store |
| `SupportChat` | Conversation ownership, agent assignment, reconnect, idle timeout, and close notifications | Redis location store |
| `DeliveryDispatch` | Courier selection, timeout reassignment, tracking, and customer push | Redis location store |
| `GameQuest` | Player quest owner Spots, event streams, and projections | Redis location store |
| `ShoppingMall` | Channel service selection, order workflow, event streams, projections, and fanout events | Redis location store |
| `ZoneWorld` | Gateway, two ZoneNodes, and Ops roles: Actor transfer across zones, zone Logical Multicast, Node direct operations, and runtime events | Redis location store |

두 언어 디렉터리 모두 이 일곱 sample root를 담는다. 내부 파일 배치는 언어마다 다를 수
있다.

```text
samples/
|-- java/
|   |-- Bingo/
|   |-- DeliveryDispatch/
|   |-- GameQuest/
|   |-- ShoppingMall/
|   |-- SupportChat/
|   |-- TicTacToe/
|   `-- ZoneWorld/
`-- kotlin/
    |-- Bingo/
    |-- DeliveryDispatch/
    |-- GameQuest/
    |-- ShoppingMall/
    |-- SupportChat/
    |-- TicTacToe/
    `-- ZoneWorld/
```

공통 sample 문서가 업무 흐름과 message 계약을 소유한다. 개별 sample README는 그 언어에
추가 설정·실행·배치 안내가 필요할 때만 있다 — 없다고 해서 지원하는 sample 목록이 바뀌지
않는다.

TicTacToe만 MeshNode peer를 손으로 구성한다. 나머지 sample은 모두 Redis location store로
Spot·Actor 위치를 찾고 MeshNode peer를 세운다.

TicTacToe에서 수동 endpoint는 연결 intent일 뿐이다. runtime이 그 endpoint를 object peer의
Redis Location Store descriptor와 맞추면, admission handshake 동안 descriptor의 RID·
lifecycle generation·보안 identity를 그대로 나른다. sample은 그 값을 직접 설정하거나 raw
transport API를 부르지 않는다.

## MeshNode And Channel Names

물리 mesh 하나에 process당 MeshNode 하나가 있다. ChannelName은 그 MeshNode 위의 논리적
service 소속일 뿐 별도 ROUTER endpoint를 만들지 않는다. Node 직접, ChannelName select-one,
Spot, Actor, Logical Multicast 연산은 모두 같은 MeshNode를 쓴다. Classic fanout만 별도
PUB/SUB channel을 쓴다.

## Project Layout

`framework/languages/java`를 IntelliJ IDEA로 열면 framework와 포함된
`zlink-framework-java-samples` Gradle build를 통해 모든 sample module을 함께 읽는다. 이
`samples/` 디렉터리를 바로 열면 sample build만 읽는다.

개별 sample 디렉터리는 자기 runner용 `standalone.settings.gradle.kts`를 쓰고 중첩
`settings.gradle.kts` root를 추가하지 않는다. 공유 message 계약은 `shared/contracts`
아래에 있다. Server topology·ChannelName·endpoint·packet·timing 설정은
`server/configuration`, client 전용 설정은 `client/configuration` 아래에 있다.

Bingo는 Protobuf payload를 쓴다. 나머지 sample은 framework의 typed JSON serialization
경로를 쓴다. sample handler와 client는 message type마다 codec을 등록하지 않는다.
