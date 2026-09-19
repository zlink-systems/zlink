[English](./framework-java-0.18.1.md) | [한국어](./framework-java-0.18.1.ko.md)

# ZLink Java Framework 0.18.1 릴리스 노트

Framework 0.18.1는 binding 1.2.1과 Core 1.2.0을 사용합니다. Framework 언어별 릴리스는 독립적으로 버전이 지정됩니다.

## 계약 변경

공개 API는 바뀌지 않습니다.

## 공통 변경

- 배포 zip(`zlink-tutorial-java.zip`, `zlink-samples-java.zip`)이 저장소 없이 빌드·실행됩니다. 각 zip 루트에 `README.ko.md`·`README.md`가 있고, 전제 조건·내려받기와 설치·빌드·실행·검증·문제 해결 절을 갖습니다. CI guard `standalone-zips`가 checkout 없는 job에서 그 README의 명령 블록을 그대로 실행합니다. (#655, #669)
- Framework GitHub Release마다 tutorial 4·samples 4, 여덟 zip을 자산으로 첨부합니다. core·binding release도 같은 자산을 싣습니다. (#639)
- Java·Kotlin tutorial에 .NET과 같은 Instance Spot 대기열(`MatchQueue`)을 더했습니다. (#666)
- 샘플 runner에서 Python 의존을 없앴습니다. ZoneWorld ZW-B8 proxy는 `SessionRouteBlockProxy`, 포트 예약은 `ReservePorts`로 Java 프로그램이 맡습니다. (#673)
- 가이드에 일곱 샘플의 따라 읽기 장(50~56)을 두고, 01·03장 코드를 tutorial snippet 참조로 바꿨습니다. (#640, #641)

## 수정

- 대기자가 자기가 관찰한 연결이 끝나는 순간 `Disconnected`로 끝납니다. 이전에는 다음 연결이 성립할 때 끝나서, 끊긴 뒤 재연결이 없으면 timeout까지 매달렸습니다(스펙 32 §10.1.1). (#667)
- `installDist`가 만드는 Windows `bin/<app>.bat`가 jar를 한 줄에 나열해 cmd.exe의 8191자 한도를 넘겨 "입력 파일이 너무 깁니다"로 죽던 것을 고쳤습니다(Kotlin tutorial 69개 jar). classpath를 `lib/*`로 씁니다. (#655)
- 샘플 runner가 ripgrep(`rg`)을 전제해 순정 Ubuntu에서 `rg: command not found`로 죽던 것을 POSIX `grep`으로 바꿨습니다. Ubuntu 기본 awk(mawk)에서 Redis container id 추출이 항상 실패하던 것도 고쳤습니다. (#655)
- 아홉 샘플의 runner가 배포 zip 안에서도 framework jar를 다시 빌드하려 하던 것을 고쳤습니다. 저장소 밖에서는 Maven Central의 패키지만 씁니다. (#655)
- Bingo·DeliveryDispatch·ShoppingMall·GameQuest·ZoneWorld 샘플(Java·Kotlin)을 계약 정본에 맞췄습니다. Bingo는 `Yield` 뒤 membership을 다시 확인하고 disconnect는 로그만 남깁니다. DeliveryDispatch는 HTTP→dispatch channel을 one-way로 보내고 sweeper 실패를 관측합니다. ShoppingMall은 terminal Instance Spot을 close하고 terminal 중복 continuation을 막습니다. GameQuest는 GameplayStateStore snapshot의 kill count를 씁니다. ZoneWorld는 incoming border loop를 configure에서 구성하고 payload의 toZone filter를 없앴습니다. (#658, #662, #663, #664, #665)

## 설치

```kotlin
implementation("systems.zlink:zlink-framework-core:0.18.1")
```

릴리스 태그는 [`framework-java/v0.18.1`](https://github.com/zlink-systems/zlink/releases/tag/framework-java%2Fv0.18.1)입니다.
