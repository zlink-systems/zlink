# C++ SubmitAdmission E2E feature map

기준 문서는
[`Config 13 — One-way submit admission`](../../../../doc/framework/common/e2e/config-13-submit-admission.ko.md)이다.
Runner는 C++ Framework가 C++ binding source를 직접 참조하지 않도록 격리된 `ZLINK_LOCAL_PACKAGE_ROOT`에
최신 `core/build` runtime을 포함한 C++ binding candidate를 먼저 만든다. Candidate에 포함된 native library의
SHA-256과 ELF Build ID가 `core/build`와 다르면 Framework를 build하지 않는다. `Caller`, `MeshTarget`,
`FanoutPublisher`는 서로 다른 process로 실행한다.

`부분 구현`은 public 결과와 process 경계 일부를 검증하지만 공통 scenario가 요구하는 gate 또는 내부 observer를
모두 제공하지 못한다는 뜻이다. 이 상태는 해당 scenario의 완료 증거가 아니다. `미구현` selector를 직접 지정하면
runner는 성공으로 건너뛰지 않고 종료 코드 3을 반환한다.

최신 Core가 요구하는 bound-session generation은 C++ binding public API와 Framework relay 경로에서
손실 없이 전달한다. Runner가 만드는 candidate package는 이 계약으로 binding과 Framework를 함께 compile한다.
기존 10.6.0 local package는 native Build ID가 최신 `core/build`와 다르므로 process E2E 완료 증거로
사용하지 않는다.

| 시나리오 | 상태 | 근거 또는 blocker |
|---|---|---|
| SA-E2E-01 | 부분 구현 | Ready 상태인 local·remote Node direct와 ChannelName call이 public `submit()` 한 번으로 `Submitted`가 되는지 실제 process에서 확인한다. Scheduler enqueue, transport attempt와 commit observer가 없다. |
| SA-E2E-02 | 미구현 | Focused unit fixture는 signal 한 번당 retry 한 번과 operation 사이의 retry credit 격리를 검증한다. Process topology에는 TCP read를 중단하는 `ReceiverGate`와 send-ready signal·retry attempt observer가 아직 없다. 고정 sleep이나 반복 submit으로 대체하지 않는다. |
| SA-E2E-03 | 미구현 | Focused unit fixture는 ready·in-flight retry 동안에도 pending reservation을 유지하고, capacity 초과 call이 transport를 한 번 시도한 뒤 `Backpressured`가 되는지 검증한다. Process topology에는 pending marker와 transport attempt observer가 아직 없다. |
| SA-E2E-04 | 미구현 | Focused unit fixture는 timeout terminal 뒤 readiness signal이 late admission을 만들지 않는지 검증하고 MeshNode startup은 `1..INT_MAX` millisecond 범위를 검사한다. Process topology에는 deterministic pending gate, deadline evidence와 validation case matrix가 아직 없다. Timeout을 늘려 통과시키지 않는다. |
| SA-E2E-05 | 부분 구현 | Production Node direct가 Location descriptor를 먼저 조회해 unknown RID와 Object Client RID를 `TargetNotFound`로 분류하고, 알려진 non-client target의 ready connection 부재는 `RouteNotConnected`로 유지한다. Unknown·known-disconnected·Object Client를 실제 process에서 대조하는 runner evidence는 아직 없다. |
| SA-E2E-06 | 미구현 | Runtime owner epoch는 stop/start 경계를 넘은 pending operation이 새 lifecycle에 waiter를 만들지 못하게 한다. Process topology에는 source admission barrier, drain admission-closed marker와 source 종료 후에도 결과를 보관하는 독립 `EvidenceCollector`가 아직 없다. |
| SA-E2E-07 | 미구현 | C++ cancellation은 공개 계약상 해당하지 않지만 Logical Multicast의 commit 전·후 shutdown 경계는 검증해야 한다. Executor barrier와 Core call observer가 없다. |
| SA-E2E-08 | 부분 구현 | Self RID와 remote RID의 즉시 수락이 같은 public call을 사용하고 기존 application mailbox admission·drain accounting을 거쳐 handler를 한 번 실행하는지 확인한다. Object Client RID는 descriptor 분류에서 `TargetNotFound`로 끝나도록 production runtime을 연결했다. Pending 뒤 수락·deadline과 Object Client actual-process 대조 evidence는 아직 없다. |
| SA-E2E-09 | 부분 구현 | Positive weight target 하나를 선택한 ChannelName call이 `Submitted`이고 해당 target handler가 한 번 완료되는지 확인한다. Pending deadline과 선택한 member observer가 없다. |
| SA-E2E-10 | 미구현 | ClientServer 역할 process와 DEALER deadline evidence가 없다. RouteMesh Channel로 대체하지 않는다. |
| SA-E2E-11 | 미구현 | Spot location generation, stale handle과 local·remote mailbox gate를 같은 runner에 연결해야 한다. |
| SA-E2E-12 | 미구현 | Actor owner generation, stale handle과 local·remote mailbox gate를 같은 runner에 연결해야 한다. |
| SA-E2E-13 | 미구현 | Logical Multicast는 target별 결과와 publish 전용 monitoring을 제공하지 않는다. Executor direct-handoff barrier, zero-target 정상 완료, partial delivery 뒤 전체 rollback·retry가 없다는 process evidence와 제거된 snapshot·metric·runtime event의 부재 검증이 아직 없다. |
| SA-E2E-14 | 구현 | Subscriber process를 시작하지 않은 publisher가 public fanout call을 한 번 submit하고 `Submitted` terminal 한 번을 반환한다. |
| SA-E2E-15 | 미구현 | Bound session·session Actor relay 역할, binding generation과 local·remote gate가 없다. |
| SA-E2E-16 | 미구현 | Server STREAM session, connector peer와 wire sequence evidence가 없다. |
| SA-E2E-17 | 미구현 | Focused STREAM unit fixture는 같은 request에서 만든 reply call 두 개 중 첫 terminator만 token을 claim하고 wire reply를 한 번 기록하는지 검증한다. 실제 peer request, concurrent terminator barrier와 process token claim observer는 아직 없다. |
| SA-E2E-18 | 미구현 | Direct target generation과 select-one의 attempt별 eligible member를 기록하는 observer가 없다. |
| SA-E2E-19 | 미구현 | Focused unit fixture는 timeout 뒤 signal과 runtime owner epoch 변경 뒤 late waiter 생성을 차단한다. Timeout·shutdown terminal 이후 connection generation과 late transport attempt를 함께 기록하는 process topology는 아직 없다. |
| SA-E2E-20 | 부분 구현 | Remote Node direct handler gate가 닫힌 동안 submit terminal이 먼저 `Submitted`가 되고 handler 완료는 gate를 연 뒤 한 번 기록되는지 확인한다. 나머지 family matrix가 없다. |
| SA-REG-01 | 구현 | 격리 package mode로 설치한 Framework header에서 async `submit()` positive compile과 제거한 동기 terminator compile-negative를 실행하고 공통 verifier의 public no-hit를 확인한다. |
| SA-REG-02 | 구현 | 고정 allowlist verifier와 `test_cpp_framework_messaging`을 실행하여 최초 `DONT_WAIT`, signal별 retry 한 번, timeout 뒤 late admission 0과 shutdown cleanup을 확인한다. |
| SA-REG-03 | 해당 없음 | Kotlin 전용 result 보존 scenario다. Runner는 C++ public 계약에 따른 N/A evidence를 남긴다. |
| SA-REG-04 | 미구현 | Focused unit fixture는 timeout·shutdown·retry terminal 뒤 waiter reservation이 0인지 확인한다. Process disposal과 send-ready event를 같은 barrier에서 발생시키고 waiter·reservation·callback 수를 확인할 observer는 아직 없다. |

## 실행

```bash
# 현재 구현 또는 부분 구현으로 표시한 process scenario와 회귀를 실행한다.
./framework/languages/cpp/e2e/SubmitAdmission/run_e2e.sh all

# 하나의 scenario만 실행한다.
./framework/languages/cpp/e2e/SubmitAdmission/run_e2e.sh SA-E2E-14
```

`all`은 위 표에서 실제 assertion과 evidence가 있는 항목만 실행한다. 공통 완료 gate는 `SA-E2E-01~20`과
`SA-REG-01~04`가 모두 구현된 뒤에만 충족한다.
