[English](./framework-java-0.22.0.md) | [한국어](./framework-java-0.22.0.ko.md)

# ZLink Java·Kotlin Framework 0.22.0 릴리스 노트

Framework 0.22.0은 binding 1.2.1과 Core 1.2.0을 사용합니다. Framework 언어별 릴리스는 독립적으로 버전이 지정됩니다.

## 계약 변경

- 생성 codec 네 언어가 service-wire terminal failure taxonomy(`terminal-failure-integrity`)를 lowering 규칙 하나로 검사합니다. boundary failure(timedOut·unavailable 등)는 `failureCode=none`, typed framework failure는 failureCode별로 정해진 terminalResult만 허용하며, 어긋난 조합(예: `timedOut`+`requestFailed`)은 encode·decode 모두에서 거부합니다. 호출자 opt-in은 없습니다. (#783)
- `relocationLogicalStreamFormat.replay`가 다섯 field의 object(`mode`·`wholeStreamInputAllocation`·`completion`·`incompleteFinalChunk`·`bytesAfterRoot`)가 되고, 생성 logical-stream decoder는 chunk를 순서대로 받는 incremental state machine입니다. complete encoded stream 입력 buffer를 만들지 않고, 성공은 final chunk에서만, final chunk에서 미완성이면 truncation, root 뒤 byte는 trailing-bytes로 실패합니다. one-shot decode API는 같은 machine의 final-chunk wrapper입니다. (#778)
- `terminal-failure-integrity.fields`가 검사 대상 owner의 유일한 목록이 되어 `creation-operation-terminal-v1.failureCode`를 포함하며, validator가 이 목록이 `terminalResult`·`failureCode` 쌍을 선언한 owner 전체와 같은지 검사합니다. 생성 codec의 동작은 같습니다. (#871)

## 공통 변경

- C++ tutorial·quickstart·samples의 `bootstrap.cmake`가 기본으로 Conan(ConanCenter prebuilt binaries)을 사용해 서드파티 라이브러리를 받습니다. 새 머신에서 약 2.5분이면 빌드가 끝납니다(vcpkg 소스 빌드 20분). vcpkg는 `-DZLINK_PACKAGE_MANAGER=vcpkg`로 선택합니다. 의존성 목록의 소유자는 `packaging/conan/conanfile.py`이며 redis-plus-plus는 1.3.15입니다. (#854)
- 포매터 범위가 `framework/languages/<lang>` 전체 소스(http-client·stream-connector 포함)로 넓어졌고, google-java-format 1.36.1이 `--aosp --skip-reflowing-long-strings`로 돕니다. (#858)
- framework release workflow가 패키지가 레지스트리에 실제로 제공된 뒤 tutorial CI를 언어별로 호출합니다. 버전 bump PR의 tutorial CI는 pin 파일 변경에 반응하지 않습니다. (#862)
- tutorial CI의 C++ job이 Conan package cache를 Actions cache에 두고, bootstrap이 성공했을 때만 저장합니다. (#852)
- Java sample runner가 lifecycle 종료 검사 실패 시 각 노드 로그의 마지막 200줄과 READY/TERMINATION marker를 stdout에 남기고, examples-smoke가 실행 디렉터리를 artifact로 보존합니다. (#829)
- 문서 사이트 deploy는 `main`에서만 실행됩니다. (#844)

## Java·Kotlin 변경

- channel·route·mesh dispatcher가 reply write 실패를 `reply_path_missing`/`drop`으로 기록합니다(네 경로). (#860)

## 설치

```kotlin
dependencies {
    implementation("systems.zlink:zlink-framework-core:0.22.0")
    implementation("systems.zlink:zlink-http-client:0.22.0")
}
```

릴리스 태그는 [`framework-java/v0.22.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-java%2Fv0.22.0)입니다.
