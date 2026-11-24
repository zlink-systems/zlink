[English](./framework-java-0.33.0.md) | [한국어](./framework-java-0.33.0.ko.md)

# ZLink Java·Kotlin Framework 0.33.0 릴리스 노트

Framework 0.33.0은 Java·Kotlin binding 1.26.0과 Core 1.26.0을 사용합니다. (#4846)

## 호환성 변경

- Spot·Actor handler owner type은 등록한 구체 type과 같아야 합니다. 다르면 명시 등록과 scan 등록 모두 startup 설정 오류입니다. 0.32.x까지 Java handler catalog는 하위 type 등록을 허용했습니다. 공통 부모 type handler는 구체 type별 handler로 나누고 공통 로직은 일반 메서드로 호출하십시오. (#4548, #4617)
- Spot은 자기 lifecycle callback(Join·Leave 등) 안에서 자기 close를 호출할 수 없습니다. 호출 시점에 InvalidOperation으로 끝납니다. callback에서는 짧은 timer를 등록하고 timer handler에서 close를 기다리지 않고 호출합니다. sample Bingo(다섯 언어)와 Node TicTacToe가 이 방식을 씁니다. (#4432, #4709, #4801)
- Java typed JSON은 `@Nullable`(JSpecify)이 보이지 않는 참조 field를 non-null로 판정합니다. null을 보내거나 받으면 직렬화·역직렬화가 실패하므로 nullable field에 `@Nullable`을 붙이십시오(Kotlin은 `?` 타입). JSpecify는 Framework core가 runtime에 함께 전달합니다. (#4412, #4515, #4798)
- 공개 계약이 없는 실행 제어를 internal bridge 뒤로 옮기고 Kotlin API snapshot을 CI에서 검사합니다. JVM API snapshot에 공개 API 세 개의 추가를 기록했습니다. (#4747, #4767, #4105, #4178)
- channel header를 JSON에서 protobuf(proto3)로 바꿨습니다. wire 형식이 바뀌므로 0.33.0 노드와 0.32.x 이하 노드는 같은 mesh에서 함께 쓸 수 없습니다. 혼합하면 hello에서 거절됩니다(required capability framework-service-v13 → v14). mesh의 모든 노드를 함께 올리십시오. (#4352, #4713)
- 서버 Framework의 라이선스가 zlink License 1.0에서 Elastic License 2.0으로 바뀝니다. client connector, 엔진 adapter, http-client, sample, template의 Apache-2.0과 Core·binding의 MPL-2.0은 그대로입니다. (#3928, #4697)

## 동작 변경

- Core 1.26.0이 `zlink_disconnect`는 connect 등록만, `zlink_unbind`는 bind 등록만 끝내도록 고칩니다. 교체된 node의 이전 descriptor endpoint를 disconnect하면 같은 endpoint에 bind한 새 node의 listener와 inbound 연결까지 닫히던 문제가 사라집니다. (#4846, #4864)
- 등록된 RouteMesh·ClientServer·automatic fanout의 status 조회와 관찰이 runtime 시작 전·시작 중에도 snapshot(`Starting`, IsReady=false)을 반환합니다. 이전에는 언어마다 InvalidOperation 등으로 끝났습니다. 시작 중 status 예외를 잡던 readiness 코드는 지워도 됩니다. (#4800, #4812)
- 닫힌·비활성 target Spot으로 가는 Actor Join은 네 언어 모두 Unavailable로 거절합니다. resolver가 Closing authority를 확인한 Actor Join은 그 사이 activation이 사라져도 NotFound로 끝납니다. (#4510, #4577, #4825, #4831)
- caller 취소·deadline 결과는 local admission을 돌려준 뒤에 공개하고, 수락된 request는 FIFO 자리에서 admission을 다시 얻어 실행합니다. (#4286, #4676)
- 공개 call은 terminal의 host admission 한 곳에서 시작 전에는 InvalidOperation, 정지 뒤에는 ShuttingDown으로 끝납니다. lease admission 거절은 ShuttingDown으로 분류합니다. (#4205, #4266, #3877, #4199)
- Actor request·send는 local admission 정리 뒤에 caller 결과를 공개하고, deadline도 dispatch 취소·정리 뒤에 공개합니다. (#3877, #4281)
- 같은 노드 bind도 Actor owner의 중복 bind 판정을 거칩니다. (#4501, #4516)
- runtime weight setter는 값만 기록하고 descriptor 게시는 renew 일정이 맡습니다. Serving이면 즉시 한 번 게시를 요청하며 표시는 게시가 확정될 때까지 남습니다. (#4858, #4861, #4893)
- Instance Spot send·request 실패가 message flow에 target_rid·reason·오류 상세를 남깁니다. (#4291, #4712)
- 소유 서버 정보가 아직 없는 request는 남은 시간 안에서 discovery 갱신을 기다립니다. Location authority scan은 요청 kind로 provider를 좁히고 page 상한·진행 규칙을 네 언어에서 맞춥니다. (#4160, #4307, #4260, #4720)
- 같은 owner에서 다시 만든 Actor로 간 ID direct request를 current Ready가 처리합니다. (#4520, #4601)
- 원격 Entry Spot join이 대상 descriptor의 lifecycle generation을 사용합니다. authority가 확정한 owner 세대를 request admission에 넘기고 authority 판정을 먼저 합니다. Actor request deadline이 위치 조회 시간을 포함합니다. (#4461, #4791, #4335, #4407, #4543, #4592)
- session bind는 entry deadline 하나를 유지합니다. authority route를 주기 전체 Store scan으로 다시 만들지 않고 scan prefix를 Store까지 전달합니다. 만료 peer 정리는 disconnect NOT_FOUND를 이미 종료로 봅니다. (#4160, #4182, #4260, #4268, #4301, #4311)
- 버려진 Actor·Spot의 authority를 정리하고, lease가 outage 밖에서 stale로 확인된 renew는 실행을 폐기합니다. route의 첫 admission snapshot을 HELLO·ADMIT가 함께 써 peer가 CONNECTING에 남지 않습니다. (#4644, #4625, #4518, #4728)
- Location Store outage 동안의 소유권 처리를 추가합니다. owner lease TTL은 30초이고 release 대기는 세 margin을 따릅니다. outage 보존 옵션·episode 관찰, 보존 gate와 T 경과 뒤 폐기, 복구 뒤 보존 객체 재개, outage 중 lease를 정상 만료 뒤에만 다시 얻는 규칙, Session fence 사본 갱신, 두 번째 outage의 새 episode 처리를 포함합니다. (#3877, #3906, #3935, #3970, #3990, #4008, #4022, #4033, #4044, #4211, #4238)
- etcd Location Store provider를 추가합니다. (#4080, #4106)

## 결함 수정

- caller가 timeout·취소한 뒤에도 target queue가 수락한 request는 원래 payload로 실행됩니다. 이전에는 caller 쪽 종료 때 payload를 먼저 닫아 수락된 작업이 빈 payload로 handler에 들어갔습니다. (#4825, #4827)
- STREAM recv가 socket state lane에서 send를 기다리지 않습니다. stream session을 routing id bytes로 찾고 public sessionId는 한 번만 만듭니다. (#4568, #4748, #4572)
- 같은 owner의 후속 incarnation으로 가는 Instance 메시지를 cold activation이 아니라 owner queue로 받습니다. (#3505, #4483)
- framework 오류의 문자열 표현이 kind와 submit 원인을 담습니다. (#4489, #4495)
- testkit fake mesh send가 공개 channel header 형식으로 header를 만듭니다. (#4352, #4844)
- sample의 nullable field에 `@Nullable`을 붙입니다(ShoppingMall·TicTacToe 메시지, morpg-fields-instances PartyCommandResult). (#4412, #4802, #4839)

## 성능 개선

- typed JSON 중복 property 검사를 객체별 bitmask로 바꿔 1K decode의 B/op이 10,688에서 6,024로 줄었습니다. (#4354, #4830)
- typed JSON bytes를 Base64 왕복 없이 decode합니다. (#4354, #4419)
- queue에 들어간 source 결과를 caller가 lane을 넘겨받지 않고 기다립니다. (#4355, #4788)
- envelope header를 parser 한 번으로 decode하고, 등록 시 준비한 이름과 canonical header reader를 쓰며, 불필요한 복사·재검증을 없앱니다. correlation id는 16-byte 배열 한 번의 hex 변환으로 만듭니다. (#3870, #3893, #3952, #4352, #4414, #4445)
- request dispatch를 bind 시점에 준비하고 binding frame을 유지하며, 최종 typed receiver를 제출 전에 bind합니다. (#3871, #3981, #3989)

## 의존성

- Java Framework core의 의존성은 shaded라 POM 변화가 없습니다. JSpecify는 typed JSON profile package가 runtime에 함께 전달합니다. (#4713, #4798)

## 설치

```kotlin
dependencies {
    implementation("systems.zlink:zlink-framework-core:0.33.0")
    implementation("systems.zlink:zlink-http-client:0.33.0")
}
```

릴리스 태그는 [`framework-java/v0.33.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-java%2Fv0.33.0)입니다.
