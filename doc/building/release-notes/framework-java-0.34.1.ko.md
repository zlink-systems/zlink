[English](./framework-java-0.34.1.md) | [한국어](./framework-java-0.34.1.ko.md)

# ZLink Java Framework 0.34.1 릴리스 노트

Framework 0.34.1은 0.34.0 템플릿을 막던 결함을 고치는 hotfix입니다. Java binding 1.26.0과 Core 1.26.0을 그대로 사용합니다.

## 결함 수정

- Play 교체 직후 같은 ID로 `getOrCreate`를 부르면 원인과 관계없이 `InternalFailure`로 끝나 다시 시도해도 되는지 알 수 없었습니다. 이제 Actor 생성 실패가 원래 원인의 종류(예: `Unavailable`)를 그대로 돌려줍니다. (#5461, #5492)
- 메시지를 JSON으로 직렬화하다 실패하면(nullable로 선언하지 않은 필드가 `null`인 경우 등) 그 실패가 message-flow 로그와 stderr에 남지 않았습니다. 이제 `zlink.dispatch_error`(`reason=encode_error`)로 기록하고, 오류 문구에 메시지 타입·필드 경로와 `@Nullable` 안내를 담습니다. routed one-way 핸들러와 decode 실패도 기록합니다. (#5454, #5501)
- 같은 SpotWide에 길이가 다른 Actor ID(`p3-…`, `capacity-p4-…` 등)가 함께 있으면 계획된 Host relocation이 target에서 "Session routes are not canonical"로 거절되었습니다. 이제 participant 순서의 Session route를 받아들이고 ActorId 중복만 검사합니다. relocation target이 PREPARE에 실패하면 실패한 단계와 원래 예외를 기록합니다. (#5473, #5515, #5504)
- 클라이언트가 `send` 직후 연결을 닫으면 Session이 이미 받은 메시지를 bound Actor로 전달하지 못하고 버렸습니다. 이제 연결 종료 전에 수락한 메시지를 Actor로 넘긴 뒤 disconnect 통지를 보냅니다(lifecycle deadline까지). (#5483, #5511)

## 설치

```kotlin
dependencies {
    implementation("systems.zlink:zlink-framework-core:0.34.1")
    implementation("systems.zlink:zlink-http-client:0.34.1")
}
```

릴리스 태그는 [`framework-java/v0.34.1`](https://github.com/zlink-systems/zlink/releases/tag/framework-java%2Fv0.34.1)입니다.
