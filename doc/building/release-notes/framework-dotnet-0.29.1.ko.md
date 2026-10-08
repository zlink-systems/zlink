[English](./framework-dotnet-0.29.1.md) | [한국어](./framework-dotnet-0.29.1.ko.md)

# ZLink .NET Framework 0.29.1 릴리스 노트

Framework 0.29.1은 .NET binding 1.17.0과 Core 1.17.0을 사용합니다.

## 결함 수정

- 같은 Instance Spot을 깨우는 operation이 활성화 도중에 도착하면 `Unavailable`·`stale_target`으로 실패하던 0.29.0 회귀를 수정했습니다. 뒤에 도착한 operation은 진행 중인 활성화에 합류하고, Ready 뒤 도착 순서대로 처리됩니다. (#1571)
- 같은 대상의 활성화에 합류한 operation이 부하에서 도착 순서와 다르게 처리될 수 있던 결함을 수정했습니다. (#1571)
- Owner node가 강제 종료된 Actor를 다시 `Create`·`GetOrCreate`하면 `location owner lease is unavailable`로 계속 실패하던 결함을 수정했습니다. Factory 등록에서 relocation을 끈(`DisableRelocation`) Actor type은 owner lease가 끝난 기존 record를 해제하고 새 incarnation을 만듭니다. Relocation 정책이 켜진 type은 계속 `Unavailable`을 반환합니다. (#1570)

## 설치

```xml
<PackageReference Include="Zlink.Framework" Version="0.29.1" />
<PackageReference Include="Zlink.HttpClient" Version="0.29.1" />
```

릴리스 태그는 [`framework-dotnet/v0.29.1`](https://github.com/zlink-systems/zlink/releases/tag/framework-dotnet%2Fv0.29.1)입니다.
[한국어](./framework-dotnet-0.29.1.ko.md) | [English](./framework-dotnet-0.29.1.md)
