# 감사 루프(R1~R5) 이월 목록

기준: 5라운드 posted 기반 리팩토링 감사(2026-08-17~18) 종료 시점.
수정 완결된 항목은 커밋 이력 참조. 이 문서는 **구조적 사유로 이월된 항목**만 기록한다.
frozen 서버 스펙(`framework/doc/framework/common/spec/server/`)은 판정 근거이며 수정 대상이 아니다.

## 언어 공통 (교차 언어 결정 필요)

- **[medium] standalone stream connector에 diagnostics level 표면 부재** — 전 언어 공통으로 커넥터 outbound가 flow를 무조건 생성·부착(사실상 always-on). spec 27 §4의 Off 규칙을 적용할 public 지렛대가 없음. 공개 API 결정 필요.
- **[medium] 커넥터 one-way Send의 correlation id** — .NET/Java/C++는 고의 부착(trace/echo), Node만 미부착. spec 27 §2 정합을 위해 wire 결정 필요(언어 단독 변경 금지).
- **[low] malformed channel envelope의 dispatch_error(invalid_frame) 기록 여부** — .NET은 peer에 protocol error reply(기록 없음), Node는 drop(기록 없음). 공통 결정 필요.
- **[low] actor-direct outbound frame의 flow pair 미탑재** — dotnet/java/node 동일(언어 패리티 상태). bound-session push의 `sent` terminal 미기록 포함.

## Java

- **[medium/구조] spot publish/direct-outbound APPLICATION flow 배선 부재** — R4에서 배선 시도가 Bingo teardown 회귀(FORCE_STOPPED)를 유발해 롤백(05f7451a57). 원인: spot dispatch lane은 turn 반환 stage의 terminal 완료가 lane 해제·drain의 전제인데, flow scope 배선이 publish turn의 반환 stage를 "래퍼 + 타 executor 완료 홉" 체인으로 바꿔 teardown 중 완료 홉이 소실되면 `awaitQuiescence()`가 영구 대기.
  권고 R1(권장): scope-wrapping 대신 **value-passing** — capture한 flow State를 encode에 명시 인자로 전달, turn 반환 stage는 bare admission future 유지. R2: publish turn을 admission 시점 lane 해제로 고정. R3: `propagateCurrent`/`withCurrentOutbound`의 RejectedExecutionException 시 래퍼 예외 완료.
- **[medium] spot route error reply의 kind 전면 보존** — 현재 PROTOCOL_ERROR만 보존(R5), 나머지는 INTERNAL_FAILURE로 평탄화. 전면 보존은 `isStaleRoute`가 NOT_FOUND를 stale-route 제어 신호로 겸용하는 구조 분리(framework-origin 마커 또는 reserved kind)가 선행돼야 하며 wire 계약 변경.
- [low] actor packet header flow id 재검증 없음(관측 오염 한정), context 이탈 deferred reply의 신규 APPLICATION flow(스펙 허용).

## Node

- **[medium/구조] `flow-context.ts currentOrCreateFlow`의 `enterWith` 영속 설치** — top-level 첫 outbound가 만든 flow가 앱 async context에 영구 잔존, 무관 작업이 flow_id 공유. 올바른 수정은 모든 outbound 진입점(채널 6+개 파일, stream frame factory, native fallback)의 `runWithFlow` 스코프 재구조화. 부분 수정은 source측 flow 소실을 만들므로 일괄 창구에서 처리.
- [low] relay JSON 대문자 flowOrigin(Node↔Node 전용 wire), 기본 생성자 binding runtime의 Off 게이트 미배선, shutdown drop frame 기록 부재, `unexpected_reply` reason 부정확, sampleRate<1의 hop 일괄 선택 약화, spot-remote-route-codec의 Off relay body 읽기, begin() sampling과 trace 시점 ambient 불일치 창.

## .NET

- **[medium] 채널 outbound `sent`/`reply_received` terminal 부재** — Java/Node는 방출, .NET만 actor 계열에 한정(spec 26 언어 간 동일성). 다중 표면 exactly-once 설계 + 교차 언어 정합 검증 필요. 대상: `ZLinkFrameworkRuntimeChannels.cs`, `ZLinkRouteClient.cs`, ClientServer client runtime.
- [low] Off의 outbound encode당 AsyncLocal read 1회, `ZLinkTelemetry` process-global level(last-writer-wins), `ConfiguredLevel` non-volatile, SpotActivationDispatcher 거절 경로의 `validateFlow:false` 상수(ON에서 malformed flow가 ProtocolError 대신 거절 사유).

## C++

- **[low/구조] flow 보존이 필요한 콜드 내부 경로의 capture 게이트 미배선** — `public_host_runtime.cpp`(509·789·1898·4307), `raw_stateful_dispatch.cpp`(572·1397), `service_wire_codec.cpp` 내부 2곳은 dispatch options 핸들이 없어 기본(검증 유지). Off에서 비정상 flow 프레임만 거부. 권고: 생성 경로에 flow-capture provider 스레딩.
- [low] stream wire 헤더의 Off 시 36B flow materialize(혼합 모드 복사 비용), 콜드패스 trace 사전 조립, detached bound-session 스테이지의 detailed 이중 문자열 조립, `relayed_frames` 무한(dispatcher 미설정 창구 한정), stream 동기 dispatch의 동일 lane 동기 재진입 미지원(문서화 권고), core-error-frame 말미 await 취약성, 미펜스 generation 헬퍼(테스트 전용), filter-chain shared_ptr 수명, request_erased 50ms 폴링, M6 이중 컴파일.

## 환경/기타

- C++ `test_cpp_framework_m6a_runtime` 결정적 실패는 2026-08-17 재설치된 core 0.11.1 패키지 기인(프레임워크 무관, stash 베이스라인으로 확정).
- 알려진 flake 목록과 known-broken 사용자 영역은 감사 대상에서 제외(메모리 `zlink-env-test-quirks` 참조).
