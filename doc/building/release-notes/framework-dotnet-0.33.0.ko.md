[English](./framework-dotnet-0.33.0.md) | [한국어](./framework-dotnet-0.33.0.ko.md)

# ZLink .NET Framework 0.33.0 릴리스 노트

Framework 0.33.0은 .NET binding 1.26.0과 Core 1.26.0을 사용합니다. (#4846)

## 호환성 변경

- Spot·Actor handler owner type은 등록한 구체 type과 같아야 합니다. 다르면 명시 등록과 scan 등록 모두 startup 설정 오류입니다. 0.32.x까지 .NET scan 등록은 불일치 handler를 조용히 제외했습니다. 공통 부모 type handler는 구체 type별 handler로 나누고 공통 로직은 일반 메서드로 호출하십시오. (#4548, #4617)
- Spot은 자기 lifecycle callback(Join·Leave 등) 안에서 자기 close를 호출할 수 없습니다. 호출 시점에 InvalidOperation으로 끝납니다. callback에서는 짧은 timer를 등록하고 timer handler에서 close를 기다리지 않고 호출합니다. sample Bingo(다섯 언어)와 Node TicTacToe가 이 방식을 씁니다. (#4432, #4709, #4801)
- channel header를 JSON에서 protobuf(proto3)로 바꿨습니다. wire 형식이 바뀌므로 0.33.0 노드와 0.32.x 이하 노드는 같은 mesh에서 함께 쓸 수 없습니다. 혼합하면 hello에서 거절됩니다(required capability framework-service-v13 → v14). mesh의 모든 노드를 함께 올리십시오. (#4352, #4713)
- 서버 Framework의 라이선스가 zlink License 1.0에서 Elastic License 2.0으로 바뀝니다. client connector, 엔진 adapter, http-client, sample, template의 Apache-2.0과 Core·binding의 MPL-2.0은 그대로입니다. (#3928, #4697)

## 동작 변경

- Core 1.26.0이 `zlink_disconnect`는 connect 등록만, `zlink_unbind`는 bind 등록만 끝내도록 고칩니다. 교체된 node의 이전 descriptor endpoint를 disconnect하면 같은 endpoint에 bind한 새 node의 listener와 inbound 연결까지 닫히던 문제가 사라집니다. (#4846, #4864)
- 등록된 RouteMesh·ClientServer·automatic fanout의 status 조회와 관찰이 runtime 시작 전·시작 중에도 snapshot(`Starting`, IsReady=false)을 반환합니다. 이전에는 언어마다 InvalidOperation 등으로 끝났습니다. 시작 중 status 예외를 잡던 readiness 코드는 지워도 됩니다. (#4800, #4812)
- Actor가 destroy돼도 그 Actor가 이미 시작한 handler turn은 재개되어 끝납니다. 새 payload만 거절되며, 재개 거절 원인이 kind·메시지로 구분됩니다(실행권 상실은 NotFound, seal·Spot 종료는 ShuttingDown). (#4800, #4806)
- 닫힌·비활성 target Spot으로 가는 Actor Join은 네 언어 모두 Unavailable로 거절합니다. resolver가 Closing authority를 확인한 Actor Join은 그 사이 activation이 사라져도 NotFound로 끝납니다. (#4510, #4577, #4825, #4831)
- caller 취소·deadline 결과는 local admission을 돌려준 뒤에 공개하고, 수락된 request는 FIFO 자리에서 admission을 다시 얻어 실행합니다. (#4286, #4676)
- 공개 call은 runtime 시작 전에는 terminal에서 InvalidOperation, 정지 뒤에는 ShuttingDown으로 끝납니다. send·publish call은 admission이 받은 뒤에만 사용한 것으로 표시합니다. (#4205, #4251, #4295)
- typed JSON 입력 계약을 지키고 네 언어 공통 fixture로 고정합니다. (#4412, #4515)
- Instance Spot send·request 실패가 message flow에 target_rid·reason·오류 상세를 남깁니다. (#4291, #4712)
- 소유 서버 정보가 아직 없는 request는 남은 시간 안에서 discovery 갱신을 기다립니다. Location authority scan은 요청 kind로 provider를 좁히고 page 상한·진행 규칙을 네 언어에서 맞춥니다. (#4160, #4307, #4260, #4720)
- 같은 owner에서 다시 만든 Actor로 간 ID direct request를 current Ready가 처리합니다. (#4520, #4601)
- Spot direct message는 current Ready incarnation이 수락하고 authority 관측이 route cache를 무효화합니다. Close 중 같은 owner의 새 Ready incarnation으로 온 Instance 메시지는 Reserve가 아니라 owner activation으로 받습니다. (#4506, #4528, #3505, #4488)
- authority 판정을 route 판정보다 먼저 하고 request는 다른 세대 peer에서 기다립니다. Actor create·Instance Spot은 authority 세대를 기다립니다. Actor request deadline이 위치 조회 시간을 포함합니다. (#4335, #4401, #4428, #4543, #4592)
- session bind는 entry deadline 하나를 유지합니다. 버려진 Actor·Spot의 authority를 정리합니다. route의 첫 admission snapshot을 HELLO·ADMIT가 함께 써 peer가 CONNECTING에 남지 않습니다. (#4160, #4182, #4644, #4518, #4728)
- 중복 bind callback은 Actor lifecycle lane과 Spot execution gate turn에서 실행됩니다. (#4771, #4784)
- 실행권을 잃은 대기 작업은 정리 순서와 관계없이 NotFound로 끝납니다. 정지한 객체의 대기 작업은 실패가 아니라 취소됩니다. Ready를 기다리는 ClientServer send는 shutdown에서 ShuttingDown으로 끝납니다. (#4371, #4567, #3977, #3979, #4030, #4036)
- Location Store outage 동안의 소유권 처리를 추가합니다. owner lease TTL은 30초이고 release 대기는 세 margin을 따릅니다. outage 보존 옵션·episode 관찰, 보존 gate와 T 경과 뒤 폐기, 복구 뒤 보존 객체 재개, outage 중 lease를 lifetime과 margin 뒤에만 다시 얻는 규칙, 새 handler turn을 막는 lease admission deadline 기록, 두 번째 outage의 새 episode 처리를 포함합니다. (#3877, #3908, #3942, #3967, #3987, #4045, #4203, #4231)
- etcd Location Store provider를 추가합니다(`Zlink.Framework.Locations.Etcd`). (#4079, #4102)

## 결함 수정

- context Close가 호출 안에서 Spot lifecycle에 등록되고, 합류 대기는 caller 취소를 따릅니다. (#4497, #4505)
- membership callback 재시도 대기 중 Spot gate를 반납합니다. suspended handler가 application worker를 쥐지 않습니다. (#4770, #4781, #4448, #4618)
- fixed RID claim에서 진 host의 startup 실패 알림이 generation 정리보다 늦어도 전달됩니다. (#4635, #4636)
- Framework가 만든 Message wrapper를 모든 종료 경로에서 닫습니다. (#4590, #4602)
- deadline index request 경로가 admission 결과를 버리지 않고 소비합니다. envelope header cache가 handler 문맥에서 lane을 동기로 기다리지 않습니다. Spot stop-source 취소를 폐기와 직렬화합니다. (#4456, #4469, #4460, #4485, #3877, #4004)
- TicTacToe Configuration 라이브러리를 일반 SDK로 바꿔 SDK 11에서도 빌드됩니다. (#4438, #4479)

## 성능 개선

- handler 결과 어댑터를 invoker에 컴파일하고 lane 동기 turn을 한 경로로 합니다. 호출당 40–80 B가 줄었습니다. (#4356, #4832)
- idle state-lane post를 queue 항목 없이 inline으로 실행합니다. 변하지 않은 RouteMesh status 조회가 후보 graph를 만들지 않고, mesh status를 한 번만 투영합니다. (#4355, #4785, #4643, #4648, #4590, #4603)
- 기본 typed JSON의 중간 JSON 배열과 Base64 임시 buffer 복사를 없앱니다. 서비스 wire head의 중복 할당과 길이 순회를 없애고 envelope header 변환 반복을 줄입니다. (#4354, #4418, #4353, #4467, #4434)
- request wire를 native Message에 직접 쓰고, ready node request를 selector·operation turn 한 번으로 제출하며, prepared dispatch와 worker 즉시 배정·binding Reply 직접 전달, RouteMesh channel handler의 mailbox claim turn 실행, request deadline·reply decode 비용 감소를 포함합니다. (#3869, #3899, #3936, #3953, #3985, #3995)

## 의존성

- `Zlink.Framework`가 Google.Protobuf 3.30.0에 의존합니다. `Zlink.Framework.AspNetCore`에도 Google.Protobuf 3.30.0 의존이 추가되고, `Zlink.Framework.Codecs.Protobuf`와 `Zlink.Framework.Locations.Etcd`의 Google.Protobuf은 3.29.1에서 3.30.0으로 올라갑니다. (#4713)

## 설치

```xml
<PackageReference Include="Zlink.Framework" Version="0.33.0" />
<PackageReference Include="Zlink.HttpClient" Version="0.33.0" />
```

릴리스 태그는 [`framework-dotnet/v0.33.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-dotnet%2Fv0.33.0)입니다.
