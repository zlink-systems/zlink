[English](./framework-java-0.32.0.md) | [한국어](./framework-java-0.32.0.ko.md)

# ZLink Java·Kotlin Framework 0.32.0 릴리스 노트

Framework 0.32.0은 Java·Kotlin binding 1.23.0과 Core 1.23.0을 사용합니다. (#3843)

## 동작 변경

- 알 수 없는 control packet이나 heartbeat payload version을 받아도 연결을 닫지 않습니다. (#3773, #3790)
- bound Session에서 Actor로 relay된 message를 도착 순서대로 전달합니다. Actor owner가 중복·역순 session sequence를 거부하고, stale-relay record의 `source_rid`에는 session owner node를 기록합니다. (#3849, #3852, #1721, #3763, #3799)
- handler executor는 모든 작업을 실행하거나 거부해야 합니다. 작업을 조용히 버리는 executor는 등록할 때 거부합니다. (#3710, #3791, #3815)
- `MessageContext.receivedAt`은 Framework가 message를 받은 UTC 시각입니다. 기본값이 없는 DTO property는 framework-json-v1에서 필수이며, 중복 envelope header field는 거부됩니다. (#2029, #3602, #1705, #3592, #3376, #3396)
- 중복 bind는 owner Actor의 `onDuplicateBind` callback이 판정합니다. (#2205, #3640)

## 결함 수정

- 늦은 retire가 교체된 Session binding을 유지하고, callback이 없는 중복 bind를 바로 수락합니다. (#3726, #3694, #3737)
- lane turn 안에서 다른 owner로 보내는 submission을 등록한 뒤 반환합니다. duplicate-bind callback이 없는 Kotlin suspend Actor는 Actor gate를 건너뜁니다. (#3417, #3562, #3694, #3750)
- 사용자 handler stage를 Framework 취소와 분리합니다. SpotNode는 lane에 합류하는 지연 조회 대신 MeshNode를 사용해 생성합니다. (#1673, #3411, #3572)
- 늦게 도착한 physical 결과는 fence가 일치할 때만 route cache 항목을 무효화하며, cache 무효화는 08-routing §2의 trigger 표를 따릅니다. 강제 종료 뒤 Store 정리는 deadline 하나를 쓰고 authority를 기준으로 sweep하며, 대상 lifecycle이 끝나면 reservation을 해제합니다. (#3571, #3771, #2989, #3409, #2646, #3462, #3583, #3697)
- relocation terminal 결과를 message flow에 기록하고 cutover flow와 Bingo join·observer 정책을 보존합니다. (#2465, #3621)
- callback 실패에서 원래 exception을 보존합니다. correlation을 읽을 수 있는 잘못된 request에는 terminal protocol error를 응답합니다. (#2098, #3429, #2512, #3418)
- Instance-intent request는 admission 전에 stale route를 거부하고 request deadline 안에서 현재 authority를 따릅니다. disconnect가 실패해도 liveness 정리가 peer 정리와 통지를 마칩니다. (#3353, #3442, #2370, #3434)
- Auto-connect는 같은 endpoint의 descriptor만 자기 자신으로 제외하며, 게시가 끝난 뒤 peer를 표시합니다. binding 완료 뒤 도착한 terminal reply는 Actor slot을 해제하고 Session reply로 보냅니다. (#2155, #3433, #3569, #3664, #2757, #3419)

## 성능 개선

- envelope header를 전체 복사 없이 한 번 파싱합니다. receive rotation은 state-lane turn 하나에서 작업을 예약하고, 고정 대상 route는 게시된 selector view에서 읽습니다. (#3613, #1673, #3402, #3416)
- send payload는 보관된 view를 사용해 native part 수를 줄입니다. Spring prototype handler는 미리 준비한 생성 경로를 재사용합니다. (#1673, #3487, #3480)

## 시험과 CI

- 시험 주입용 constructor를 package-private으로 두고 Java·C++ 시험은 범위를 제한한 access seam으로 internals를 확인합니다. (#2309, #3796, #3834)

## 알려진 제한

- Java `mmo-world-channels` template update E2E는 간헐적으로 STOPPED 뒤에도 `sessionB`를 종료하지 못합니다. (#3596)

## 설치

```kotlin
dependencies {
    implementation("systems.zlink:zlink-framework-core:0.32.0")
    implementation("systems.zlink:zlink-http-client:0.32.0")
}
```

릴리스 태그는 [`framework-java/v0.32.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-java%2Fv0.32.0)입니다.
