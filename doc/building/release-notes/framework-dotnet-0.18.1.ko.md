[English](./framework-dotnet-0.18.1.md) | [한국어](./framework-dotnet-0.18.1.ko.md)

# ZLink .NET Framework 0.18.1 릴리스 노트

Framework 0.18.1는 binding 1.2.1과 Core 1.2.0을 사용합니다. Framework 언어별 릴리스는 독립적으로 버전이 지정됩니다.

## 계약 변경

공개 API는 바뀌지 않습니다.

## 공통 변경

- 배포 zip(`zlink-tutorial-dotnet.zip`, `zlink-samples-dotnet.zip`)이 저장소 없이 빌드·실행됩니다. 각 zip 루트에 `README.ko.md`·`README.md`가 있고, 전제 조건·내려받기와 설치·빌드·실행·검증·문제 해결 절을 갖습니다. CI guard `standalone-zips`가 checkout 없는 job에서 그 README의 명령 블록을 그대로 실행합니다. (#655, #669)
- Framework GitHub Release마다 tutorial 4·samples 4, 여덟 zip을 자산으로 첨부합니다. core·binding release도 같은 자산을 싣습니다. (#639)
- 샘플 runner에서 Python 의존을 없앴습니다. 역할 설정 JSON은 bash heredoc으로 쓰고, ZoneWorld ZW-B8 proxy는 의존성 없는 `net8.0` 콘솔 프로젝트로 바꿨습니다. (#673)
- 가이드에 일곱 샘플의 따라 읽기 장(50~56)을 두고, 01·03장 코드를 tutorial snippet 참조로 바꿨습니다. (#640, #641)

## 수정

- binding 1.2.1이 NuGet 패키지에 `runtimes/win-x64/native/zlink.dll`을 실어, Windows에서 배포 패키지만으로 동작합니다. 1.2.0은 Linux x64 runtime만 담고 있어 Windows에서 `DllNotFoundException`이 났습니다. (#702)
- 대기자가 자기가 관찰한 연결이 끝나는 순간 `Disconnected`로 끝납니다. 이전에는 다음 연결이 성립할 때 끝나서, 끊긴 뒤 재연결이 없으면 timeout까지 매달렸습니다. `Close`도 대기자를 같은 방식으로 풉니다(스펙 32 §10.1.1). (#667)
- 배포 samples zip의 `sample_runner.ps1`이 저장소 전용 `local_nuget.ps1`을 무조건 읽어 Windows에서 일곱 샘플이 모두 죽던 것을 고쳤습니다. 저장소 밖에서는 nuget.org의 `Zlink.Framework` 패키지만 참조합니다. (#655)
- Bingo·TicTacToe·SupportChat 샘플을 계약 정본에 맞췄습니다. Bingo Session callback은 bound Actor를 순회하거나 binding을 직접 제거하지 않습니다. TicTacToe Api·Play는 고정 RID를 쓰고 반대 뜻의 known-deviation 주석을 지웠습니다. SupportChat의 Actor factory는 모두 `DisableRelocation`을 고르고 Relocation Store를 등록하지 않습니다. (#658, #659, #660)
- tutorial README의 빌드와 실행 구성을 Release로 통일했습니다. Debug로 빌드하고 Release 경로를 실행하던 불일치입니다. (#655)

## 설치

```bash
dotnet add package Zlink.Framework.AspNetCore --version 0.18.1
```

릴리스 태그는 [`framework-dotnet/v0.18.1`](https://github.com/zlink-systems/zlink/releases/tag/framework-dotnet%2Fv0.18.1)입니다.
