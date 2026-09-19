[English](./framework-cpp-0.18.1.md) | [한국어](./framework-cpp-0.18.1.ko.md)

# ZLink C++ Framework 0.18.1 릴리스 노트

Framework 0.18.1는 binding 1.2.0과 Core 1.2.0을 사용합니다. Framework 언어별 릴리스는 독립적으로 버전이 지정됩니다.

## 계약 변경

공개 API는 바뀌지 않습니다.

## 공통 변경

- 배포 zip(`zlink-tutorial-cpp.zip`, `zlink-samples-cpp.zip`)이 저장소 없이 빌드·실행됩니다. `bootstrap.cmake`가 GitHub Release의 Core prebuilt, C++ binding 소스, framework 소스 세 아카이브를 받아 설치하고, runner는 저장소 트리와 배포 트리를 구분합니다. 각 zip 루트에 `README.ko.md`·`README.md`가 있고, 전제 조건·내려받기와 설치·빌드·실행·검증·문제 해결 절을 갖습니다. CI guard `standalone-zips`가 checkout 없는 job에서 그 README의 명령 블록을 그대로 실행합니다. (#655, #669)
- Framework GitHub Release마다 tutorial 4·samples 4, 여덟 zip을 자산으로 첨부합니다. core·binding release도 같은 자산을 싣습니다. (#639)
- C++ tutorial에 .NET과 같은 Instance Spot 대기열(`MatchQueue`)을 더했습니다. Client는 `/match-queues/{mode}`의 첫 요청으로 queue를 cold activate합니다. (#666)
- 샘플 runner에서 Python 의존을 없앴습니다. 포트 선택·역할 설정·JSON은 bash로, ZoneWorld ZW-B8 proxy는 C++ 프로그램으로 바꿨습니다. (#673)
- 가이드에 일곱 샘플의 따라 읽기 장(50~56)을 두고, 01·03장 코드를 tutorial snippet 참조로 바꿨습니다. (#640, #641)

## 수정

- 대기자가 자기가 관찰한 연결이 끝나는 순간 `Disconnected`로 끝납니다. 이전에는 끊김에서 대기자를 풀지 않고 `close`에서만 풀어, 재연결이 없으면 timeout까지 매달렸습니다. heartbeat timeout·read pump·`dispatch_pending`·`receive_next`·`wait_for_packet`·`submit_wait_async` 여섯 자리가 한 함수 `connection_ended`를 부릅니다(스펙 32 §10.1.1). (#667)
- Instance Spot handler가 자기 turn 안에서 `close()`를 부르면 그 turn의 정상 응답이 `requestFailed`(terminal 105)로 덮이던 것을 고쳤습니다. 수락한 turn의 종료 지점을 handler 반환에서 응답 전송 뒤로 옮겨, close는 새 admission만 막고 accepted turn은 응답을 보낸 뒤 한 번 종료합니다(스펙 §6.2). (#692)
- `bootstrap.cmake`가 Ninja가 없는 환경에서 generator를 정하지 않아 `CMAKE_MAKE_PROGRAM is not set`으로 죽던 것을 고쳤습니다. Unix Makefiles를 고르고 vcpkg baseline을 shallow fetch합니다. (#655)
- Ubuntu 기본 awk(mawk)에서 Redis container id 추출이 항상 실패하던 것과, README Windows 블록의 stdout 상속·PowerShell 문자열 문제를 고쳤습니다. (#655)
- Bingo·TicTacToe·SupportChat 샘플을 계약 정본에 맞췄습니다. Bingo Session mesh는 고정 RID 대신 역할 prefix의 자동 RID를 쓰고, Session disconnect callback은 bound Actor를 순회하거나 통지하지 않습니다. TicTacToe Play StreamNode는 `enable_actor_dispatch()`를 켭니다. SupportChat Entry Spot은 상담원 disconnect에 availability를 내리고, ConversationId metadata를 allow-list에 올리며, 메시지를 발신자를 뺀 참가자에게 push합니다. (#658, #659, #660)

## 설치

[`framework-cpp/v0.18.1` GitHub Release](https://github.com/zlink-systems/zlink/releases/tag/framework-cpp%2Fv0.18.1)에서 `zlink-framework-cpp-0.18.1.tar.gz`를 내려받고 `find_package(zlink_framework CONFIG REQUIRED)`를 사용합니다. vcpkg overlay port와 Conan recipe로도 설치할 수 있습니다.
