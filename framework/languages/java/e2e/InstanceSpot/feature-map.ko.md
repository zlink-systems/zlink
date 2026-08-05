# Java·Kotlin Instance Spot E2E feature map

이 문서는 공통 E2E Config 14의 Java·Kotlin 구현 차이를 기록한다. 현재 `IS-*`
시나리오는 실행 fixture와 runner가 없으므로 완료 증거로 사용하지 않는다. 후속
`S8-IS-JVM`에서 정식 spec에 맞는 public API, runtime, contract test와 process E2E를 함께
구현한다.

| Scenario | 상태 | 남은 검증 |
|----------|------|-----------|
| `IS-R01` | 미구현 | Core·bindings에 Instance activation service 표면이 남지 않음 |
| `IS-R02` | 미구현 | 공개 raw socket API만으로 multipart·RID·monitor·bounded I/O·close 구현 |
| `IS-R03` | 미구현 | Binding private symbol·reflection·generated service symbol·Core private header 미사용 |
| `IS-P01` | 미구현 | Activation recovery envelope의 identity·fence·first packet·deadline 보존 |
| `IS-P02` | 미구현 | Target descriptor·owner lease exact fence와 단일 CAS winner |
| `IS-P03` | 미구현 | Activation message·byte 상한, FIFO와 request terminal-once |
| `IS-P04` | 미구현 | Durable first record와 Ready CAS 사이 handler barrier |
| `IS-P05` | 미구현 | Stable wire failure code와 네 decoder parity |
| `IS-P06` | 미구현 | Close·expiry·abort·host terminal 뒤 resource cleanup |
| `IS-P07` | 미구현 | Activation crash 단계별 orphan·reservation recovery |
| `IS-P08` | 미구현 | Exact generation·lease 확인 뒤 activation failure release |
| `IS-P09` | 미구현 | First handler terminal·replay cursor 뒤 recovery pointer release |
| `IS-C01` | 미구현 | Driver SPI symbol·layout·status prefix |
| `IS-C02` | 미구현 | Cold placement·activation data 보존 |
| `IS-C03` | 미구현 | Local registry kind 충돌·single create |
| `IS-C04` | 미구현 | Leader/follower claim·token single-consume |
| `IS-C05` | 미구현 | Bounded activation pending queue |
| `IS-C06` | 미구현 | Ready 이후 cold first-message ordering |
| `IS-C07` | 미구현 | Abort·deadline·shutdown terminal |
| `IS-C08` | 미구현 | Begin-close·monotonic admission deadline |
| `IS-C09` | 미구현 | Actor lifecycle target 금지 |
| `IS-C10` | 미구현 | Activation status·pending metric |
| `IS-C11` | 미구현 | Final candidate C contract suite |
| `IS-B01` | 미구현 | Framework driver symbol projection |
| `IS-B02` | 미구현 | Kind·state·placement·token value projection |
| `IS-B03` | 미구현 | Token ownership·reuse rejection |
| `IS-B04` | 미구현 | Core result mapping |
| `IS-B05` | 미구현 | Candidate native ABI·hash contract |
| `IS-F01` | 미구현 | 언어 공통 public API |
| `IS-F02` | 미구현 | Factory registry·option validation |
| `IS-F03` | 미구현 | Canonical descriptor type set |
| `IS-F04` | 미구현 | Eligible serving node selector |
| `IS-F05` | 미구현 | Address coordinator·redirect boundary |
| `IS-F06` | 미구현 | Location Store CAS·Redis·change stamp |
| `IS-F07` | 미구현 | Placement token·leader target dispatch |
| `IS-F08` | 미구현 | Lifecycle·DI·Ready commit ordering |
| `IS-F09` | 미구현 | Failure cleanup exactly once |
| `IS-F10` | 미구현 | CAS loser 외 재제출 금지 |
| `IS-F11` | 미구현 | Lease fencing·monotonic deadline |
| `IS-F12` | 미구현 | Caller-driven expired-row takeover |
| `IS-F13` | 미구현 | Drain seal·Closing·release ordering |
| `IS-F14` | 미구현 | Bounded observability labels |
| `IS-F15` | 미구현 | Reference sample·process E2E |
| `IS-REG-01` | 미구현 | Missing SpotHandle가 activation을 시작하지 않는 회귀 |
| `IS-REG-02` | 미구현 | 기존 exact generation fencing 회귀 |
| `IS-REG-03` | 미구현 | Domain Spot local creation 비변경 회귀 |
| `IS-REG-04` | 미구현 | Entry Spot startup·shutdown 비변경 회귀 |
| `IS-REG-05` | 미구현 | Domain Spot Actor membership 비변경 회귀 |
| `IS-REG-06` | 미구현 | Domain drain과 Instance row 격리 회귀 |
| `IS-REG-07` | 미구현 | Channel messaging·reply correlation 비변경 회귀 |
| `IS-REG-08` | 미구현 | 기존 resolver의 Instance 전이 row 차단 회귀 |
| `IS-REG-09` | 미구현 | Domain·Instance kind 충돌 회귀 |
| `IS-REG-10` | 미구현 | Ready owner one-way exact route 회귀 |
| `IS-REG-11` | 미구현 | Existing request deadline·cancellation 비변경 회귀 |
| `IS-REG-12` | 미구현 | Application public surface와 driver 격리 회귀 |
| `IS-REG-13` | 미구현 | Multi-mesh identity·queue·generation 격리 회귀 |
| `IS-REG-14` | 미구현 | Instance one-way async-only submit 회귀 |
| `IS-E2E-01` | 미구현 | Cold request claim·factory·Ready·reply |
| `IS-E2E-02` | 미구현 | Cold send submit 경계와 activation 결과 관측 |
| `IS-E2E-03` | 미구현 | 동시 최초 호출의 단일 owner·factory·handler 실행 |
| `IS-E2E-04` | 미구현 | 서로 다른 주소의 분산과 독립 serial queue |
| `IS-E2E-05` | 미구현 | Ready owner crash와 lease 뒤 takeover |
| `IS-E2E-06` | 미구현 | Activating owner crash와 다음 call activation |
| `IS-E2E-07` | 미구현 | Drain 제외·accepted turn·release·재활성화 순서 |
| `IS-E2E-08` | 미구현 | Close·release·새 generation 활성화 |
| `IS-E2E-09` | 미구현 | 만료 row 동시 takeover의 단일 CAS winner |
| `IS-E2E-10` | 미구현 | Stale owner의 message·timer·Store 전이 차단 |
| `IS-E2E-11` | 미구현 | 확정된 target 미수락에서 request 재제출 금지 |
| `IS-E2E-12` | 미구현 | 수락 여부가 불확실한 request 재제출 금지 |
| `IS-E2E-13` | 미구현 | 수락된 send 종료의 무재생과 drop 관측 |
| `IS-E2E-14` | 미구현 | Store 장애와 owner deadline fencing |
| `IS-E2E-15` | 미구현 | Kind·type 충돌과 기존 owner 유지 |
| `IS-E2E-16` | 미구현 | Eligible node 없음과 row 잔여 0 |
| `IS-E2E-17` | 미구현 | Activation message·byte backpressure |
| `IS-E2E-18` | 미구현 | 교차 언어 결과·오류·timeout parity |
| `IS-E2E-19` | 미구현 | Ready-visible ordering |
| `IS-E2E-20` | 미구현 | Closing owner crash와 stale release 차단 |
| `IS-E2E-21` | 미구현 | Multi-mesh source·배선·row 격리 |
| `IS-E2E-22` | 미구현 | Core monotonic owner deadline fencing |
| `IS-E2E-23` | 미구현 | Actor·Logical Multicast capability 금지 |
| `IS-E2E-24` | 미구현 | 늦은 Store renew 응답의 deadline 재개방 금지 |
| `IS-E2E-25` | 미구현 | Ready 뒤 mark-ready 실패 cleanup |
| `IS-E2E-26` | 미구현 | Concurrent owner claim leader·follower 정리 |
| `IS-E2E-27` | 미구현 | Call deadline과 shared activation 격리 |
| `IS-E2E-28` | 미구현 | Core close seal과 Closing CAS 경쟁 |
| `IS-E2E-29` | 미구현 | Cross-mesh in-flight drain completion 순서 |
| `IS-E2E-30` | 미구현 | Multi-mesh 동시 drain terminal 단일 완료 |
| `IS-E2E-31` | 미구현 | Remote CAS loser exact route redirect 1회 |
| `IS-E2E-32` | 미구현 | Ready owner의 close와 다음 generation activation |
| `IS-E2E-33` | 미구현 | Activation 중복 요청의 단일 terminal과 cleanup |
| `IS-E2E-34` | 미구현 | Host shutdown 중 activation deadline과 release |
| `IS-E2E-35` | 미구현 | Cross-language activation 결과와 오류 parity |
| `IS-E2E-36` | 미구현 | Instance activation process E2E와 role evidence |
