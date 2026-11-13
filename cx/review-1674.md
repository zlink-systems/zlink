# #1674 .NET 리뷰 (감독자, 2026-10-15) — m1674c 9 커밋 (origin/main..2f6d00b855), 27 files
판정: 수정 뒤 승인. 스펙 gap 0(아래 반려 반영 시).
- bcd97695bc G1 승인: SpotNodes는 InitializeSpotNodesAsync 안에서만 Add/Remove(ZLinkSpotNodeInitializer.cs:116,191), 종료는 읽기만 — lane 밖 읽기 안전. 실패 문자열 지연 생성.
- ef24fe5a98 G2 반려: IsSealedObserved(lane 밖 volatile)는 IsSealed(lane turn)와 같은 사실의 두 번째 판정 수단(AGENTS §3 execution gate 우회 금지 목록) + 판정 위치 1→2(supervisor-guide §2 반려 기준). 주석이 인정하듯 seal 뒤 record 하나가 더 시작될 수 있다. 세 언어는 record마다 lane 판정. → 커밋을 되돌리고 새 MeshNodeRouteBatchAdmissionTests는 회귀 시험으로 남긴다(수정 전후 모두 통과하는 시험).
- e604acd1b6 G4 승인: static lambda + state tuple, admission 규칙 불변.
- fc41c5c5b5 G3 승인: LaneWork 항목이 결과 신호 소유 — closure 두 겹 제거, turn 순서·drain claim·posted fault 경로 불변. W3 record마다 permit 반환 유지(S3 철회와 일치).
- 8ca02c7395 승인: 시험의 타입 단정만 ICollection으로, 개수 단정 불변.
- d60d0d73c4 G5 수정 요청(D-F2): dispatcher의 ConcurrentDictionary<Type, factory>는 DI 카탈로그 factory 표와 같은 매핑의 두 번째 사본(AGENTS §3 중복 금지, 브리프 cache 금지). 모든 scope가 같은 카탈로그를 공유하므로 카탈로그 참조를 dispatcher가 한 번 잡고 그 표를 쓴다.
- 27235d0e2d G6 승인: ZLinkSerialTurn.Scope·ZLinkRuntimeOperationScope의 Dispose는 AsyncLocal 복원뿐 — ExecutionContext.Restore 1회와 같다.
- e2a95d7c36 M1a 승인: 등록은 submit 전(E3), 원자적 꺼내기 유지, 이미 결정된 admission만 제출 호출 안에서 완료(01 §11 새 turn — continuation이 걸릴 수 없는 시점). 기존 시험은 늦은 admission 경우로 옮김(계약 변경에 따른 대상 이동), 새 시험은 수정 전 실패. S1 스펙 개정과 같은 방향.
- 2f6d00b855 M2 승인: 기존 불변 view 게시 지점에 direct peer 사실 추가, lane 위·밖 같은 view → 조회 규칙 2→1.
- 규칙 위반 기록: job이 pkill을 1회 사용(브리프 금지).
- 2026-10-15 main merge 4a642aa18d 감독자 리뷰 승인: SpotCloseConformanceTests StartPeerAsync는 main #1812(WaitForPeerAsync·실패 시 dispose) 채택, branch 쪽은 같은 호출의 인자 변경뿐이라 남길 것 없음. Unit 2988·Contract 96 실패 0.
- 머지 전 확인(zlink 요청 2026-10-15): #1769 계열(Hello 수락 시 admitted 기록 위치 변경, 스펙 #1790·#1793) 구현이 main에 들어오면 merge 때 #1817의 direct peer 게시 지점과 admitted 기록 순서를 다시 대조.
- 2026-10-15 main merge 14aec172db(#1822 #1769 포함) 감독자 리뷰 승인. 텍스트 충돌 없으나 의미 충돌 1건 수정: #1817(2f6d00b855)이 direct send가 lane turn 없이 게시된 peer view(_channelSelection.DirectPeer)를 읽게 해, #1822의 '송신자는 다음 lane turn에 선택' 전제가 깨짐 → CompletePeerAdmissionUnderLock이 admitted 기록 → Admit 제출(SubmitAdmissionResponseUnderLock, TrySendExactControlAsync는 첫 await 전에 binding에 제출) → Ready·peer view 게시·event 순으로. 03-mesh-node.ko.md:338-339 §7.1 순서 유지, 판정 위치 하나(:9299), #1822 전용 loop thread 그대로(해당 파일 diff 0). 이 창을 재현하는 시험은 없음(코드 순서 근거). Unit 2992·Contract 96 실패 0(mainmerge-1015b/).
- zlink 재리뷰 승인(2026-10-15 11:18): CompletePeerAdmissionUnderLock hunk.
- 2026-10-15 main merge b3d428cf7a(#1825 #1761 .NET) 감독자 리뷰 승인: RequireDirectPeer 충돌 — branch의 lane 밖 view 읽기 유지 + main의 lifecycle generation fence를 view 판정에 포함(:9929). relocation prepare는 chunk fence용 peer 객체를 lane turn 1회로 읽고(main도 1회, per-message 아님) admission·generation은 RequireDirectPeer 한 규칙; 둘 사이에 peer가 바뀌면 SendCanonicalRelocationRecordAsync의 객체 fence가 NotConnected → durable replay. 수신 판정(HasCurrentSource :5737)과 송신 대상 판정(view)은 서로 다른 사실이라 판정 위치 중복 아님. admission 순서 :9276→:9280→:9282 유지. 시험 1건 reflection delegate 시그니처만 변경(단언 불변). Unit 3005·Contract 96 실패 0(mainmerge-1015c/). 참고: .NET은 아직 모든 send를 등록(S1 미적용, 후속).
- 2026-10-15 5c36422c84 감독자 리뷰 승인(zlink 반려 반영): command 40과 chunk가 lane에서 읽은 peer 객체의 물리 route 하나로 나가고, view route가 다르면 송신 전 NotConnected → durable replay. 근거 06-wire-protocol.en.md:655-656(ko :593). 시험 1개 추가(수정 전 실패 확인), Unit 3006·Contract 96 실패 0.
- zlink 재리뷰 승인 5c36422c84 (12:28).

## 2차 리뷰 (감독자, 2026-10-16) — framework-dotnet/1674-per-message-cost-2: 1e896b5dea, a155956f15, 89d262b96e, 0ced4f5ca4, 6efeba1585
- 1e896b5dea M1b 승인: 즉시 admission은 submit 결과로 완료(table entry 없음), 나중 admission만 RegisterSend 후 Observe — S1(01 §10, PR #1821). 완료된 결과를 caller에 반환하는 것은 lock 밖, §11 위반 아님. send source monitor 4→1. 시험 이동(table→node)은 계약 위치 이동 결과.
- a155956f15 S2 승인: CaptureEnabled는 Normal 이상 한 곳, dispatch error 기록은 DispatchErrorsEnabled(Off 제외). 시험 3건 Normal로(단언 불변).
- 89d262b96e 승인: receive loop 전용 호출(Recv·monitor Recv·RoutesSnapshot)에서 _socketGate 제거 — 그 handle들은 loop 반환 뒤에만 close(Core socket README.ko.md:49-53 send 동시 허용·receive 원자, :57-59 close는 진행 중 호출에 EBUSY, :94-98 data receive 하나; 06-monitoring :142-144; 07-router :298). submit·reply·transport control·wake timer(07-utilities :231,250,273)는 gate 유지. request당 8→2.
- 0ced4f5ca4 승인: 회전 관리 상태를 node turn 하나로 수집(CollectInfrastructureUnderLock). seal은 이전에도 1회 읽고 전부 건너뜀 → break와 동치. 만료 처리 같은 turn.
- 6efeba1585 승인: PeerConnectionIntentRemoved(node lane turn 안)에서 Cancel → CancelAsync로 continuation이 turn 안에서 재진입하지 않게(01 §11). 이 교착은 2차 변경이 timing을 바꿔 드러냄(14회 중 2 hang → 8/8).
결정: (1) close 뒤 새 send는 이전처럼 ShuttingDown(06 §4 owner Close '새 제출을 막고', Java #1846과 같은 규칙) — 판정은 node lifecycle 한 곳. (3) lane turn: claim·drain 한 turn 병합, DrainReadyAsync ready set 승인; M3 HasCurrentSource는 #1825 수신 gate와 얽혀 보류. (4) CancellationToken boxing은 비취소 token에 boxing 상수 승인; local actor published view 등은 3차. 기존 결함(local node send 20000 중 1 유실, 시작점에서도 재현)은 이슈 등록.
- 머지 전 확인: main #1849(#1745 .NET Dispose 정지)가 6efeba1585와 같은 결함(intent 제거 시 lane 안 취소 continuation)을 고침 — 1674-per-message-cost-2에 main merge 때 main 수정 채택, 6efeba1585 의미 중복 제거.
- 2026-10-16 9b26061750·aaf11ed81b·8d83a76c90·8f1ff31d26 감독자 승인: CloseSubmissions(node lane+socketGate에서 _state=Stopped, pump dispose 전) → submit은 ShuttingDown(판정 node 한 곳, 06 §4). ready registry(_readyMailboxes: true=ready, false=claim 예약) — enqueue·release 모두 mailbox lane, claim 중 enqueue는 release의 '남음'으로 재게시해 신호 유실 없음, DrainReady는 TryUpdate로 예약 후 ClaimAndDrain 한 turn(FIFO prefix 캡처+drain). 비취소 token은 boxing 상수. unit 3009·contract 96 실패 0. request lane turn 12→11.
- 2026-10-16 main merge 7113a7c628 + f01c28604d(#1849와 중복된 CancelAsync 되돌림) 뒤: unit 3013 통과, 실패 2는 main에서도 나는 binding 1.19.1 회귀(BorrowedPayload count 1/2, zlink i1835b), contract 96/96. push 완료. PR은 #1817 브랜치에 main merge(#1848 진단 job이 그 worktree 사용 중 — 끝난 뒤) 후 쌓아서 연다.
- 2026-10-16 main merge b30e16e665(main 66cddc8ab3, binding 1.19.1 pin·#1849 등) 감독자 승인: 텍스트 충돌 0·수동 변경 0(remerge-diff 비어 있음). (a) CompletePeerAdmissionUnderLock :9304-9318 순서 유지, (b) relocation :1157-1170 같은 route 유지, (c) #1849 listener out-of-turn은 main 그대로, (d) RequireDirectPeer 한 곳. unit 3010 통과, 실패 2는 binding 1.19.1 이중 Dispose 회귀(zlink 1.19.2 예정), contract 96. push.
- 머지 전 확인: main .NET pin 1.19.2(#1858·#1860, 반복 Dispose 회귀 수정) — #1817·#1855에 main merge 때 함께 받고 BorrowedPayload 통과 확인.
