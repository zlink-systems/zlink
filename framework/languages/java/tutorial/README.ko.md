# ZLink Java/Kotlin Tutorial

Channel 메시징과 id로 부르는 Spot 하나를 기능별로 쌓아 가는 프로그램이다. Java와 Kotlin
두 언어를 담았고, `java/`와 `kotlin/`이 같은 Gradle build 안 subproject다.
`settings.gradle.kts`·`gradle/libs.versions.toml`·wrapper는 두 언어가 함께 쓴다.

English: [`README.md`](./README.md)

## 전제 조건

- **JDK 25.** 자동 toolchain resolver 없음 — 없으면 설치하고 `JAVA_HOME`을 그 경로로
  둔다. 자세한 내용과 오류 메시지는 [`java/README.ko.md`](./java/README.ko.md#전제-조건)를
  본다.
- **Docker Desktop.** Redis 하나를 컨테이너로 띄운다.

  ```bash
  docker run --rm -p 6379:6379 redis
  ```

## 내려받기와 설치

저장소를 checkout하지 않는다. `systems.zlink:zlink-framework-*` 패키지를 Maven Central에서
받아 빌드한다. 이 zip을 푼 디렉터리(`zlink-tutorial-java/`)가 전부다.

이 파일과 명령 모두 이 zip을 푼 디렉터리(`zlink-tutorial-java/`) 안에서, 그 디렉터리를
현재 위치로 두고 실행한다.

## 빌드

```bash title="linux"
./gradlew :java:Server:installDist :java:Client:installDist
./gradlew :kotlin:Server:installDist :kotlin:Client:installDist
```

```powershell title="windows"
.\gradlew.bat :java:Server:installDist :java:Client:installDist
.\gradlew.bat :kotlin:Server:installDist :kotlin:Client:installDist
```

## 실행

언어별로 나뉜다. Server를 먼저 실행하는 것은 둘 다 같다.

- Java: [`java/README.ko.md`](./java/README.ko.md#실행)
- Kotlin: [`kotlin/README.ko.md`](./kotlin/README.ko.md#실행)

## 검증

두 process가 서로를 받아들이면 각 언어 README의 "검증"에 있는 `PEER_READY` 로그 줄이
찍히고, 그 뒤의 curl 호출이 `200`을 낸다.

## 문제 해결

언어마다 포트·키 prefix가 달라 항목을 나눴다. Docker/Redis·JDK 관련 오류(연결 거부,
`UnsupportedClassVersionError`, `No matching toolchain found`)와 stale Redis 키 정리는
[`java/README.ko.md`](./java/README.ko.md#문제-해결)와
[`kotlin/README.ko.md`](./kotlin/README.ko.md#문제-해결)에 각각 있다.
