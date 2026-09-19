# ZLink Java/Kotlin Tutorial

A step-by-step program that layers Channel messaging and one id-addressed
Spot. Both Java and Kotlin are included, as `java/` and `kotlin/`
subprojects of the same Gradle build, sharing `settings.gradle.kts`,
`gradle/libs.versions.toml`, and the wrapper.

한국어: [`README.ko.md`](./README.ko.md)

## Prerequisites

- **JDK 25.** No auto-download toolchain resolver — install it yourself and
  point `JAVA_HOME` at it. Details and exact error text:
  [`java/README.md`](./java/README.md#prerequisites).
- **Docker Desktop.** Runs Redis as one container.

  ```bash
  docker run --rm -p 6379:6379 redis
  ```

## Download And Install

No repository checkout. Build against the `systems.zlink:zlink-framework-*`
packages from Maven Central. The directory this zip extracts to
(`zlink-tutorial-java/`) is everything you need.

## Build

```bash
cd zlink-tutorial-java
./gradlew :java:Server:installDist :java:Client:installDist
./gradlew :kotlin:Server:installDist :kotlin:Client:installDist
```

```powershell
cd zlink-tutorial-java
.\gradlew.bat :java:Server:installDist :java:Client:installDist
.\gradlew.bat :kotlin:Server:installDist :kotlin:Client:installDist
```

## Run

Split by language. Both start the Server first.

- Java: [`java/README.md`](./java/README.md#run)
- Kotlin: [`kotlin/README.md`](./kotlin/README.md#run)

## Verify

Once the two processes accept each other, the `PEER_READY` log line in each
language's README "Verify" section appears, and the curl call after it
returns `200`.

## Troubleshooting

Ports and key prefixes differ per language, so this is split too. Docker/Redis
and JDK errors (connection refused, `UnsupportedClassVersionError`,
`No matching toolchain found`) and stale-Redis-key cleanup are each in
[`java/README.md`](./java/README.md#troubleshooting) and
[`kotlin/README.md`](./kotlin/README.md#troubleshooting).
