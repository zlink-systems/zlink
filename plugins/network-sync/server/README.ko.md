<!-- SPDX-License-Identifier: LicenseRef-Zlink -->
# Network sync server plugin

`Zlink.NetworkSync.Server`는 ZLink Framework의 User Spot 하나에서 매치 하나를 돌리는 서버 plugin입니다. `Simulation/`은 Framework를 참조하지 않는 매치 틱·입력 버퍼 코어이고, `Server/`는 그 코어를 공개 Framework API(Spot timer, SpotWide 실행, typed Actor packet handler, `BoundSession.Send`)에 연결합니다. wire 형식은 [`../protocol/`](../protocol/README.ko.md)이 정합니다.

## 사용

- 매치 Spot은 `NetworkSyncSpot<TSpot, TActor, TInput, TAction, TState>`를 상속하고 `AdmitPlayerAsync`, `GetMissingInput`, `Step`, `CaptureSnapshot`을 구현합니다. 등록할 때 SpotWide 실행과 relocation 끔을 지정합니다.
- **Session dispatch에서 `NetworkSyncSession.TryRelayAsync`를 반드시 거칩니다.** 입력 byte 상한은 Session ingress에서 원래 payload 길이로만 검사합니다(typed Spot handler는 원래 바이트를 받지 못합니다). 이 호출을 거치지 않고 입력을 Actor로 넘기면 크기 검사가 빠집니다.
- 수신자마다 진행 중인 send admission 하나와 아직 보내지 않은 최신 full snapshot 하나만 둡니다. 틱은 admission을 기다리지 않으므로 느린 client가 Spot을 막지 않습니다.
- 시뮬레이션(`Step`·`CaptureSnapshot`)이 예외를 던지면 timer를 멈추고 Spot을 닫습니다.

## 검증

`dotnet test Simulation.Tests`와 `dotnet test Server.Tests`(Server.Tests는 `ZLINK_REDIS_TEST_ENDPOINT`의 Redis가 필요합니다. 기본 `127.0.0.1:6379`). CI는 `.github/workflows/network-sync-server.yml`입니다.
