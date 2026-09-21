[English](./framework-dotnet-0.21.0.md) | [한국어](./framework-dotnet-0.21.0.ko.md)

# ZLink .NET Framework 0.21.0 릴리스 노트

Framework 0.21.0은 binding 1.2.2와 Core 1.2.0을 사용합니다. Framework 언어별 릴리스는 독립적으로 버전이 지정됩니다.

## 계약 변경

- 없음. 공개 계약은 0.20.0과 같습니다.

## 공통 변경

- tutorial Server가 `GET /fanout/broadcast/ready`(200/503)로 fanout 구독 준비 상태를 알리고, tutorial CI는 그것을 기다린 뒤 broadcast를 보냅니다. fanout은 연결 전 event를 저장하지 않으므로 publish 전에 subscriber 준비를 확인해야 합니다. (#839)
- ZoneWorld `run_sample.sh`가 child runner·browser 프로세스를 단일 EXIT 정리에 포함해 실패로 끝나도 Redis 컨테이너·프로세스가 남지 않습니다. (#823)
- runtime과 공개 계약은 0.20.0과 같습니다. 이 릴리스는 tutorial·samples·quickstart와 저장소 도구를 정리합니다.
- quickstart·tutorial·samples는 언어별 examples 저장소(`zlink-<lang>-examples`)에서 받습니다. 미러 workflow가 실행 비트를 보존하고(#831), README 상단에 English | 한국어 선택 줄을 둡니다.
- tutorial CI의 C++ job이 vcpkg binary cache를 Actions cache에 둡니다. (#852)
- google-java-format 1.27.0으로 Java·Kotlin 포맷 검사가 JDK 25에서 그대로 돕니다. (#798)
- 사용되지 않던 v11 public-contract trace generator와 inventory를 제거했습니다. (#747)

## 설치

```xml
<PackageReference Include="Zlink.Framework" Version="0.21.0" />
<PackageReference Include="Zlink.HttpClient" Version="0.21.0" />
```

릴리스 태그는 [`framework-dotnet/v0.21.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-dotnet%2Fv0.21.0)입니다.
