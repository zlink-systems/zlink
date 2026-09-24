[English](./framework-node-0.24.0.md) | [한국어](./framework-node-0.24.0.ko.md)

# ZLink Node.js Framework 0.24.0 릴리스 노트

Framework 0.24.0은 binding 1.6.0과 Core 1.6.0을 사용합니다. Framework 언어별 릴리스는 독립적으로 버전이 지정됩니다.

## 계약 변경

- Fixed RID를 manual topology와 automatic discovery 모두에서 Object role과 관계없이 사용할 수 있습니다. Fixed RID와 automatic RID prefix를 함께 설정하면 startup configuration error가 발생합니다. 재시작할 때 이전 owner lease가 유효하면 즉시 conflict로 실패하고, lease가 만료되면 같은 RID를 다시 사용할 수 있습니다. (#1045, #1056)

## 공통 변경

- Core 1.5.0부터 늦게 `dlopen()`으로 읽는 host에서 static TLS 공간 부족으로 `libzlink`를 읽지 못하던 문제를 해결합니다. Framework는 binding 1.6.0을 통해 이 Core 버전을 사용합니다. (#1041)
- Core 1.6.0은 ROUTER가 요청을 받은 직후 보낸 쪽 연결이 끊기면 reply token 발행을 거부해 수신 경로가 오류로 끝나던 문제를 해결합니다(드물게 C++ process가 종료되던 원인). (#1051, #1062)
- Quickstart와 tutorial의 listener를 `127.0.0.1`로 제한했습니다. Tutorial은 기존 Redis를 사용할 때 tutorial 전용 키를 정리하는 방법과 강제 종료 후 이전 owner lease가 만료될 때까지 기다리거나 해당 키를 삭제한 뒤 재시작하는 방법을 설명합니다. (#1035, #1043, #1045, #1047, #1049)
- Engine server 예제를 `EngineLobby.sln`, `Server`, `Shared` 프로젝트로 구성했습니다. (#1036, #1039)

## 수정

- Godot C#·C++ 예제의 Windows DLL 배치, C++ 빌드 설정과 실행 안내를 수정해 Godot 4.4.1에서 engine server와 통신하도록 했습니다. (#1037, #1040)
- Unity(Windows native·WebGL)와 Cocos Creator 예제의 manifest·scene·수신값 로그·README를 고쳐 Unity 6000.0.83f1과 Cocos Creator 3.8.8에서 engine server와 통신하도록 했습니다. (#1037, #1064, #1066)
- Node tutorial의 패키지 표에서 이전 Framework 버전 표기를 고쳤습니다. HttpClient tutorial의 조회 결과 예시도 실제 응답인 `fetch speedy-p2`로 고쳤습니다. (#1035, #1043, #1013)

## Node.js 변경

- 메모리 및 Redis Location Store에서 유효한 이전 owner와 충돌한 새 RID claim을 `ignoredStale` 대신 `rejectedConflict`로 보고하도록 고쳤습니다. (#1045, #1047)
- Instance Spot을 닫은 뒤 같은 generation의 route가 다시 활성화되어 relocation이 `RelocationFailed`로 끝나던 문제를 고쳤습니다. (#1050, #1053)
- `@zlink-systems/stream-connector`가 `Set`·`Map` spread 대신 `Array.from`을 사용합니다. loose 변환을 쓰는 번들러(Cocos Creator)에서 수신 handler가 실행되지 않던 문제를 고쳤습니다. Unity WebGL UPM package에는 asset `.meta` 파일을 포함합니다. (#1037, #1064, #1066)

## 설치

```bash
npm install @zlink-systems/framework@0.24.0 @zlink-systems/http-client@0.24.0
```

릴리스 태그는 [`framework-node/v0.24.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-node%2Fv0.24.0)입니다.
