# .NET SpotService E2E feature map

기준 문서: `framework/doc/framework/common/e2e/config-2-spot-service.ko.md`

| 시나리오 | 상태 | 근거 |
|----------|------|------|
| SM-A1 | 구현 | entry spot request marker가 있다. |
| SM-A2 | 구현 | user spot state mutation marker가 있다. |
| SM-A3 | 구현 | 특정 spot id request가 owner node evidence에만 남는 route resolver marker가 있다. |
| SM-A4 | 구현 | owner routing marker가 있다. |
| SM-A5 | 구현 | app-level ScenarioStage wrapper를 통해 spot request/timer/lifecycle을 실행하고 marker를 확인한다. |
| SM-A6 | 구현 | spot initialize와 explicit close lifecycle marker가 있다. |
| SM-A7 | 구현 | spot type mismatch marker가 있다. |
| SM-A8 | 구현 | worker offload marker가 있다. |
| SM-A9 | 미구현 | User Spot factory·initialize 뒤 Location Store publication barrier와 concurrent caller 합류 E2E가 없다. |
| SM-A10 | 미구현 | Framework 발급 Entry Spot ID 형식과 lifecycle별 새 ID E2E가 없다. |
| SM-A11 | 구현 | 예약된 Entry Spot ID를 User Spot `GetOrCreate`와 Instance Spot request에 제출하고 두 호출이 모두 `InvalidOperation`으로 끝나는지 확인한다. 해당 ID와 일치하는 Location Store read·write와 User·Instance Spot factory 실행은 모두 0이다. `./run_e2e.sh sm-a11` 증거는 `logs/20260729-154511-1994689/`에 있다. |
| SM-A12 | 미구현 | User Spot automatic ID의 첫 collision 즉시 실패 E2E가 없다. |
| SM-A13 | 미구현 | SpotId UTF-8 string 경계와 legacy binary Spot RID 거부 E2E가 없다. |
| SM-B0 | 구현 | Missing ID의 manager `Find`가 factory를 실행하지 않는지 먼저 확인한다. 이어 `play-a`만 placement eligible로 두고 explicit Actor ID·stable type `Create`, duplicate `GetOrCreate(...).InMesh(...)`, final `Find`를 실행한다. 세 결과는 `Created`·`Existing`·`Found`이며 같은 Actor generation과 owner로 수렴한다. Factory 실행은 `play-a` 1회, weight 0인 `play-b` 0회다. `./run_e2e.sh sm-b0` 증거는 `logs/20260729-154921-2063035/`에 있다. |
| SM-B0A | 미구현 | Actor 생성 승인·거절과 concurrent GetOrCreate terminal result E2E가 없다. |
| SM-B1 | 구현 | local actor join marker가 있다. |
| SM-B2 | 구현 | remote actor join marker가 있다. |
| SM-B3 | 구현 | request message object fidelity marker가 있다. |
| SM-B4 | 구현 | session-a에서 play-b actor로 request 후 play-b reply marker가 있다. |
| SM-B5 | 구현 | missing actor packet marker가 있다. |
| SM-B6 | 구현 | actor leave/disconnect callback marker가 있다. |
| SM-B7 | 구현 | Created → Joined → actor packet 순서 marker가 있다. |
| SM-B8 | 구현 | explicit actor destroy marker가 있다. |
| SM-B9 | 구현 | `JoinAdmittedUserSpotActorReq`가 local/remote user spot join admission의 허용과 거부를 확인하고, 거부 actor가 user spot에 join되지 않는 evidence를 검증한다. |
| SM-B10 | 미구현 | Object role과 Location·Relocation Store startup validation E2E가 없다. |
| SM-B11 | 미구현 | Store-backed Actor publication barrier와 concurrent caller 결과 E2E가 없다. |
| SM-C1 | 구현 | channel to spot messaging marker가 있다. |
| SM-C2 | 구현 | spot to channel messaging marker가 있다. |
| SM-C3 | 구현 | spot-to-spot request/send/publish와 missing target negative marker가 있다. |
| SM-C4 | 구현 | spot publisher client marker가 있다. |
| SM-C5 | 구현 | 10.0.0 MeshNode의 play-a Spot이 발행한 Logical Multicast가 play-b 구독 Spot에 도달하는 evidence를 최신 `default-batch` 실행에서 확인했다. |
| SM-C6 | 부분 구현 | Gateway publish endpoint는 현재 result-free one-way 계약으로 전환해 제출 완료만 반환한다. 기존 client는 제거된 target별 `ZLinkPublishResult`와 부분 수락 개수를 전제로 하므로, 현재 공개 계약에서 관측할 수 있는 backpressure와 최종 전달 조건으로 scenario를 다시 정의해야 한다. |
| SM-D1 | 구현 | local actor session bind/relay marker가 있다. |
| SM-D2 | 구현 | remote actor session bind/relay marker가 있다. |
| SM-D3 | 구현 | entry spot bind와 user spot bind를 각각 stream session에 연결하고 relay/push marker를 확인한다. |
| SM-D4 | 구현 | multiple actor bind marker가 있다. |
| SM-D4A | 구현 | 같은 generation의 Session A→B rebind, 이전 binding의 typed stale relay, 늦은 disconnect 격리와 두 Session의 다른 Actor binding 유지를 실제 process에서 검증했다. `./run_e2e.sh --skip-build sm-d4a`가 통과했으며 증거는 `logs/20260728-190632-1115028/`에 있다. |
| SM-D4B | 구현 | Bind 뒤 해당 Actor의 Location Store read를 차단한 상태에서 valid request·push, rebind 뒤 stale token, active Message Follow와 만료 뒤 typed stale을 실제 process로 검증한다. Matching Store read는 모두 `0`이다. `./run_e2e.sh sm-d4b` 증거는 `logs/20260729-152459-1501571/`에 있다. |
| SM-D5 | 구현 | Session host의 local Actor와 Play host의 remote Actor를 같은 STREAM에 bind한다. Remote Actor의 logical notification callback을 실행 중인 상태에서 physical disconnect를 발생시켜 exact binding dedupe를 확인한다. Local Actor callback은 evidence 기록 뒤 실패하지만 remote callback과 Session cleanup은 all-settled로 완료된다. Bind 뒤 두 Actor의 Location Store read를 차단해 matching read `0`, callback 각 1회, ObjectGeneration과 membership 유지를 검증한다. `./run_e2e.sh sm-d5` 증거는 `logs/20260729-153632-1814746/`에 있다. |
| SM-D5A | 구현 | 같은 connection에 두 Actor를 bind하고 public `NotifyDisconnectedAsync` reply 뒤 선택 Actor callback 1회와 다른 Actor request 생존을 검증한다. `./run_e2e.sh --skip-build sm-d5a`가 통과했으며 증거는 `logs/20260725-083229-855693/`에 있다. |
| SM-D6 | 구현 | bound session push targeting marker가 있다. |
| SM-D7 | 구현 | stream auth and dispatch marker가 있다. |
| SM-D8 | 구현 | stream 연결 종료 중 pending request 실패를 확인하고 새 session에서 reauth/rebind 후 messaging 재개 marker를 확인한다. |
| SM-D9 | 구현 | stream inbound observer marker가 있다. |
| SM-D10 | 구현 | 작은 MaxReceivedMessages session에 push를 몰아 bounded drop을 확인하고, 같은 session request와 다른 session push가 계속 정상 동작하는지 검증한다. |
| SM-D11 | 구현 | 같은 run에서 stream actor request와 channel route request를 함께 수행하는 marker가 있다. |
| SM-D12 | 구현 | session reconnect migration marker가 있다. |
| SM-D13 | 구현 | heartbeat-enabled stream이 유지된 뒤 request가 성공하는 marker가 있다. |
| SM-D14 | 구현 | framework stream node의 `SetTlsServer(...)` public API로 TLS stream endpoint를 열고, TLS connector 성공 경로와 strict certificate validation 실패 경로를 함께 확인한다. |
| SM-D15 | 구현 | gateway role의 HTTP request가 `IZLinkActorClient.RequestToActor(...)`로 actor handler에 도달하고, actor가 bound stream session으로 push한 notify를 client가 수신하는지 확인한다. |
| SM-E1 | 구현 | spot route missing request marker가 있다. |
| SM-E2 | 구현 | spot timer tick marker가 있다. |
| SM-E3 | 구현 | idle timer가 spot close를 수행하고 closed spot request가 실패하는 marker가 있다. |
| SM-E4 | 구현 | timer overrun policy marker가 있다. |
| SM-F1 | 구현 | client/server channel to target spot marker가 있다. |
| SM-F2 | 구현·process 통과 | `play-b`에 배치한 User Spot을 `play-a` 역할 server가 global Spot ID만으로 request/send한다. Target handler의 request·send evidence와 source handler 0건을 확인했다. 실제 process 증거는 `logs/20260728-233321-3170089`이며 이전 local `play-a` 실행은 remote owner 증거로 사용하지 않는다. |
| SM-F3 | 구현 | client/server egress와 route-mesh egress를 같은 spot에 혼재해 처리한 marker가 있다. |
| SM-F4 | 구현 | missing target spot route request 실패 marker가 있다. malformed relay packet 주입은 public E2E 표면이 아니므로 직접 scenario로 만들지 않는다. |
| SM-F5 | 구현 | target user Spot을 public manager로 닫은 뒤 해당 Spot 경로만 실패하고 같은 route channel의 일반 request는 계속 성공하는 marker가 있다. |
| SM-F6 | 구현 | `sm-f6` runner에서 같은 MeshName의 두 MeshNode만 사용한다. Source는 global Spot ID만으로 remote request/send를 제출하고 public Actor join을 실행하며, target owner Spot의 handler evidence를 확인한다. |
| SM-G1 | 구현 | play-a를 강제 종료한 뒤 play-b 요청이 계속 성공하고, 같은 endpoint·RID로 play-a를 재기동한 뒤 gateway와 session request/reply가 모두 복구되는 것을 확인했다. 이어 play-a를 다시 종료하고 play-b가 재배치된 actor 요청을 처리하는 경로까지 통과했다. 증거: `logs/20260720-011511-1678310/`. |
| SM-G2 | 구현 | node B 추가 뒤 기존 Spot·actor owner 유지와 신규 Spot·actor의 명시적 node B 배치를 `sm-g2` 실행에서 확인했다. |
| SM-G3 | 구현 | 같은 user spot에 여러 stream session이 동시에 join/request/leave를 수행하고 actor별 join/leave lifecycle evidence가 1회씩 남는지 확인한다. |
| SM-G4 | 구현 | 다수 bound session에 동시에 push를 보내 각 session이 자기 actor push만 받는지 확인한다. |
| SM-G5A | 구현·process 통과 | 800개 Actor와 800개 User Spot을 실제 process에서 생성해 weight 100:300의 `play-b` 소유 비율이 65~85%인지 확인하고, 기존 Actor owner가 유지되는지 검증한다. 유효 weight 0·100·10000과 무효 weight -1·10001도 확인했다. `./run_e2e.sh --skip-build sm-g5a` 증거는 `logs/20260805-110259-1276374/`에 있다. |
| SM-G5B | 구현·process 통과 | weight 10000의 `play-b`에 stable type capacity 1을 먼저 채운 뒤 같은 type을 다시 생성해 high-weight full node가 제외되고 `play-a`로 배치되는지 실제 process reply로 확인했다. `./run_e2e.sh --skip-build sm-g5b` 증거는 `logs/20260805-110449-1282215/`에 있다. |

현재 표에서 `미구현`과 `전환 대상`인 행은 `run_e2e.sh all`의 완료 범위에 포함되지 않는다.
따라서 과거 default-batch 결과를 현행 Config 2 전체 완료 증거로 사용하지 않는다. Runtime M6
완료 뒤 각 행의 public API 시나리오와 runner registration을 추가하고 전체 selector를 다시 검증한다.
