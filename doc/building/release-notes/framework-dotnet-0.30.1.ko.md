[English](./framework-dotnet-0.30.1.md) | [한국어](./framework-dotnet-0.30.1.ko.md)

# ZLink .NET Framework 0.30.1 릴리스 노트

Framework 0.30.1은 .NET binding 1.19.2과 Core 1.19.0을 사용합니다.

## 변경

- 서버 Framework의 라이선스가 zlink License 1.0으로 바뀌며, 1.0.0 전 버전은 키나 유료 라이선스 없이 머신 수와 관계없이 무료입니다. Client connector·공용 client 코드·sample·tutorial·quickstart와 HTTP client는 Apache-2.0이며, 0.30.0 이하 릴리스의 라이선스는 바뀌지 않습니다. (#1681)
- 공개 API 추가: request 결과 `ZLinkRequest<TReply>`에 `Admitted` 단계를 제공합니다. Reply와 별도로 해당 request의 source-local 수락을 기다릴 수 있으며, 수락 전에 실패하면 두 단계가 같은 원인으로 실패합니다. (#1682)
- 메시지 송수신의 중간 payload 복사와 반복 topology 조회, Protobuf writer·request 관찰 할당을 줄이고, binding admission은 결과를 분류한 뒤 backpressure일 때만 기다립니다. Caller continuation 하나가 막혀도 다른 request completion은 진행합니다. (#1689, #1696, #1747)
- Debug에서도 runtime을 최적화하여 동기 완료한 async 호출이 불필요한 state-machine 객체를 할당하지 않도록 했습니다. (#1706)

## 결함 수정

- 잘못된 envelope를 받은 request가 언어에 따라 오류 응답 없이 끝나거나 다르게 처리되던 결함을 수정했습니다. Header가 문법에 맞는 JSON object이고 유효한 correlation ID와 reply token이 있으면 오류 응답을 보내며, reply 경로를 확인할 수 없으면 응답 없이 관측 정보에 기록합니다. (#1671)
- Spot Configure의 잘못된 handler·subscription 등록을 activation의 Ready 전에 `InvalidOperation`으로 거부하도록 네 언어의 오류 분류를 맞췄습니다. (#1715)
- Relocation이 전달하는 새 owner generation을 Store CAS 결과와 일치시키고 sealed Session binding을 함께 전달합니다. 이동 후 Session route를 확정할 때 서로 다른 generation이나 누락된 binding 상태를 사용하던 경로를 수정했습니다. (#1807)
- Actor Destroy·ownership loss·Join 정리가 겹칠 때 teardown 예외가 나거나, Message Follow route가 준비되기 전에 노출되어 거부되던 결함을 수정했습니다. 같은 incarnation의 teardown과 cutover route 사용 가능 여부를 각각 한 경계에서 확정합니다. (#1666, #1669)
- AutoConnect 시작과 종료가 겹칠 때 `NullReferenceException`이 나거나, transport 해제 뒤 peer 정리가 `ObjectDisposedException`으로 실패하던 결함을 수정했습니다. Worker token을 시작 시 확보하고 AutoConnect 종료를 transport 정리 전에 완료합니다. (#1668, #1711)
- Spot Close·successor 게시·cold activation과 shutdown이 겹칠 때 retained work가 준비되지 않은 Spot을 보거나 종료가 멈추던 결함을 수정했습니다. Close 수락·합류를 직렬화하고 수락한 activation·게시된 relocation attempt의 정리를 기다리며, 초기 Location claim 취소 시 확인된 lease를 해제합니다. (#1670, #1677, #1773, #1794)
- Permit 반환 뒤 다음 poll까지 RouteMesh send가 적체되거나 multicast child permit을 중복 확보하던 결함을 수정했습니다. Permit 반환 시 수신을 깨우고 child permit은 수신 source 하나가 확보하며, 미설정 HWM은 Core 기본값을 사용하고 request 취소는 queue waiter를 먼저 정리합니다. (#1684, #1794)
- Stream Connector 기본 JSON codec이 서버 typed JSON과 다른 형식을 사용하던 결함을 수정했습니다. 64-bit 정수 문자열, enum 이름과 필수·null·중복 field 검증을 포함해 `framework-json-v1`을 공통으로 사용합니다. (#1703)
- 물리적으로 종료한 Session 정리가 Actor unbound를 다시 보내 shutdown이 `ForceStopped/TeardownFailed`로 끝나던 결함을 수정했습니다. Transport 종료 중에는 unbound를 제출하지 않습니다. (#1708)
- 이미 수락한 handler의 `BoundSession.Send`가 relocation·shutdown seal 뒤 다시 거부되던 결함을 수정했습니다. Local Actor send도 Actor mailbox 수락과 FIFO를 거치므로, 수락 전에 성공을 반환하거나 앞선 메시지를 추월하지 않습니다. (#1709, #1744)
- Assembly에 열린 generic handler 정의가 있으면 닫힌 타입만 등록해도 node 시작이 실패하던 결함을 수정했습니다. 인스턴스를 만들 수 없는 scan 후보는 제외하며, 열린 generic의 명시 등록은 계속 구성 오류입니다. (#1736)
- 빈 reply를 가진 deferred Actor Join recovery가 metadata 누락으로 거부되던 결함을 수정했습니다. 빈 reply는 metadata 없이 복구합니다. (#1746)
- Descriptor 변경 뒤 같은 revision을 중복 Renew하여 첫 게시 성공을 stale 결과로 덮고 host 시작이 실패하던 결함을 수정했습니다. Descriptor 게시는 한 경로에서 한 번 확정합니다. (#1807)
- 여러 ClientServer host의 blocking poll로 ThreadPool이 고갈되거나 수신 중 socket close가 `EBUSY`로 실패하던 결함을 수정하여, 전용 수신 thread가 loop 종료 후 socket을 닫습니다. 정지 중 수락한 reply는 남은 drain deadline까지 송신하며, admission 기록 순서·admission 전 control 수신·첫 owner lease heartbeat 시각도 바로잡았습니다. (#1812, #1822)
- Route 교체 중 durable request와 relocation batch·cutover가 멈추던 결함을 수정하여, 다른 선택 route를 관찰하면 ready 상태에서 다시 제출합니다. Admission 전 일반 record는 응답 없이 버리고 교차 HELLO에는 최초 snapshot으로 ADMIT을 답하며, 그동안 바뀐 descriptor는 ADMIT 뒤 UPDATE로 게시합니다. (#1825, #1870)
- Target 종료 뒤 relocation authority가 `Missing`이면 source를 `DataLost`로 끝내던 동작을 expired-owner terminal로 맞췄습니다. Session route 확정에서 Session owner 소관이 아닌 authority generation 재검사도 제거했습니다. (#1847)
- 연결 intent 제거 중 취소 callback이 같은 state lane을 기다려 Dispose가 멈추거나 durable request가 `NotFound`로 끝나던 결함을 수정했습니다. 종료 사실은 intent 제거와 같은 turn에서 확정하고 외부 정리는 turn 밖에서 실행하며, lifecycle 종료 결과는 `Unavailable`로 일관되게 보고합니다. (#1849, #1861)
- .NET binding 1.19.2를 사용합니다. 작은 payload의 native 주소가 compacting GC로 무효화되어 메모리가 손상되던 결함과, 반복 Dispose·Message 재사용으로 해제된 저장소를 접근하던 회귀가 수정됐습니다. (#1841, #1858)
- 시작 검사에서 identity 없는 임시 Spot으로 `Configure()`를 호출하던 경로를 제거했습니다. 실제 activation에서 등록을 검사하므로 `Configure()`가 실제 Spot identity를 사용할 수 있습니다. (#1715)

## 설치

```xml
<PackageReference Include="Zlink.Framework" Version="0.30.1" />
<PackageReference Include="Zlink.HttpClient" Version="0.30.1" />
```

릴리스 태그는 [`framework-dotnet/v0.30.1`](https://github.com/zlink-systems/zlink/releases/tag/framework-dotnet%2Fv0.30.1)입니다.
[한국어](./framework-dotnet-0.30.1.ko.md) | [English](./framework-dotnet-0.30.1.md)
