# ZLink C++ Framework Samples

C++ 샘플은 10.0.0 framework의 공개 API로 여러 서버 역할을 구성하는 방법을 보여 준다. 업무 흐름과
검증 기준은 [공통 sample 문서](../../../doc/framework/common/sample/README.ko.md)를 따르며, C++ 코드는
runtime reflection 대신 compile-time 타입으로 handler를 등록한다.

각 서버 실행 파일은 자기 역할만 구성한다. Runner는 역할별 프로세스를 시작하고 연결 준비를 확인한 뒤
공개 client 시나리오를 실행하며, 종료할 때 자신이 시작한 프로세스와 Redis container를 정리한다.
샘플 코드가 다른 서버 역할을 같은 프로세스에서 시작하지 않는다.

## 샘플 목록

| 샘플 | 보여 주는 기능 | 연결 구성 | payload codec |
|---|---|---|---|
| `Bingo` | Session gateway, Entry Spot, room Spot, Actor binding, timer와 bound-session push | Redis location store | Protobuf |
| `TicTacToe` | API 2개와 Play 2개의 scale-out, room route 조회와 실시간 게임 | 수동 peer endpoint와 Redis room route store | JSON |
| `SupportChat` | conversation Spot, 상담 배정, reconnect, idle timer와 종료 알림 | Redis location store | JSON |
| `DeliveryDispatch` | courier 선택, timeout 재배정, tracking과 customer push | Redis location store | JSON |
| `GameQuest` | player별 quest owner Spot, event stream과 조회 모델 | Redis location store | JSON |
| `ShoppingMall` | ChannelName service, 주문 workflow, event stream과 fanout 알림 | Redis location store | JSON |

TicTacToe만 peer endpoint를 수동으로 설정한다. 다른 샘플은 Spot과 Actor의 위치를 찾고 MeshNode peer를
구성할 때 Redis location store를 사용한다. Application code가 peer 목록이나 연결 순서를 관리하지
않는다.

## MeshNode와 channel

하나의 물리 mesh는 process마다 MeshNode 하나로 구성한다. `ChannelName`은 그 MeshNode가 참여하는
논리 service group이며 별도 ROUTER endpoint를 만들지 않는다. Node direct, ChannelName select-one,
Spot, Actor와 Logical Multicast는 같은 MeshNode를 사용한다. 전 수신자에게 전달하는 classic fanout은
별도 PUB/SUB channel이다.

## 실행

Linux 또는 WSL에서 샘플 하나를 실행하려면 해당 runner를 호출한다.

```bash
./framework/languages/cpp/samples/TicTacToe/run_sample.sh
./framework/languages/cpp/samples/Bingo/run_sample.sh
```

지원하는 여섯 샘플을 순서대로 실행하려면 통합 runner를 사용한다.

```bash
./framework/languages/cpp/samples/run_samples.sh
```

DeliveryDispatch 샘플은 현재 Linux 또는 WSL용 `run_sample.sh`로 전체 client/server 흐름을 검증한다.

일부 샘플만 실행할 때는 샘플 이름을 지정한다.

```bash
./framework/languages/cpp/samples/run_samples.sh Bingo SupportChat
```

Windows PowerShell에서도 같은 책임을 가진 runner를 사용한다.

```powershell
.\framework\languages\cpp\samples\TicTacToe\run_sample.ps1
.\framework\languages\cpp\samples\run_samples.ps1
```

기본 빌드 디렉터리는 `framework/languages/cpp/build`이다. Runner는 build, 역할별 설정 파일 생성,
서버 시작, readiness 확인, client self-check와 정리를 순서대로 수행한다.

## 설정과 계약 배치

서버 역할은 설정 파일 경로를 받고, `app.config()`가 읽은 값을 typed configuration에 bind한 뒤
framework builder에 전달한다. Endpoint, Redis, routing ID, timeout과 로그 경로를 application 환경
변수로 직접 읽지 않는다. Standalone client는 직접 연결해야 하는 외부 endpoint와 timeout만 검증된
CLI option 또는 client 설정 파일로 받는다.

`Shared/Contracts`에는 client와 server가 함께 직렬화하는 message 계약만 둔다. 서버 topology,
ChannelName, endpoint 이름, packet 이름과 timing 설정은 `Server/Configuration`에 두고, client 전용
설정은 `Client/Configuration`에 둔다.

수동으로 역할 하나를 실행할 때도 역할별 설정 파일을 넘긴다.

```bash
sample_cpp_framework_tictactoe_play --config=./appsettings.play-a.json
sample_cpp_framework_tictactoe_api --config=./appsettings.api-a.json
sample_cpp_framework_tictactoe_client --api-http-endpoint=http://127.0.0.1:48113
```
