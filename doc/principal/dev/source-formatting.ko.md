# 소스 줄바꿈 규칙과 포매터

이 문서는 소스의 줄바꿈 모양과 그것을 만드는 도구를 소유한다. 규칙은 언어에 관계없이 넷이고,
그 밖의 모양은 언어별 포매터의 출력이 정한다. 손으로 접은 줄바꿈은 포매터가 되돌리므로 규칙을
따로 외울 필요가 없다 — `scripts/format/format.sh`를 실행하면 된다.

적용 범위는 현재 `framework/languages/<lang>/{quickstart,tutorial,samples}`이다. 가이드가 `--8<--`로 그
소스를 발췌하므로 줄바꿈이 곧 문서의 모양이다. 저장소 전체로 넓히는 것은 별도 작업이다.

## 1. 규칙

1. **한 줄은 100열까지다.** 100열 안에 들어가는 호출·선언·식은 한 줄로 쓴다. 인자 중간이나
   제네릭 인자 앞에서 끊거나, 짧은 식을 `+`·`<<` 앞에서 접거나, `{ get; }\n    = context`처럼
   선언을 접지 않는다.
2. **호출이 100열을 넘으면 인자마다 한 줄이다.** 인자 둘을 한 줄에 묶지 않는다(bin-packing 금지).
3. **체인이 100열을 넘으면 `.` 호출마다 한 줄이다.** 100열 안에 들어가는 체인은 호출 수와
   무관하게 한 줄이다. 넘치는 체인의 첫 호출을 receiver 줄에 붙이는지, 닫는 `)`를 어디에 두는지,
   연속 들여쓰기 폭, trailing comma는 언어 포매터의 출력을 따른다.
4. **대입의 오른쪽은 `=` 줄에서 시작한다(C++).** `auto x =` 뒤에서 끊고 다음 줄에 식을 두지 않는다.
   오른쪽이 100열을 넘으면 규칙 2·3대로 인자·`.` 호출에서 끊고, 이어지는 줄은 오른쪽 시작 열에
   연속 들여쓰기를 더한 자리다(`PenaltyBreakAssignment`, #847). C#·TypeScript는 체인을 이미
   이렇게 두고, Java·Kotlin은 google-java-format·ktfmt에 이 옵션이 없어 그 출력(`=` 뒤 줄바꿈)을
   그대로 둔다(#851 결정).

   ```cpp
   auto mesh = options.add_route_mesh ("services")
                 .listen ("tcp://0.0.0.0:7302")
                 .set_object_role (fw::object_role_t::none);
   ```

포매터 출력이 규칙이므로 `// prettier-ignore`·`// csharpier-ignore`·`// clang-format off`·
`// @formatter:off`는 그 블록이 포매터 출력으로는 읽을 수 없게 되는 경우에만 쓰고, 쓴 이유를
바로 위 주석에 적는다.

## 2. 언어별 포매터

| 언어 | 도구 | 버전 고정 위치 | 설정 |
|---|---|---|---|
| C# | CSharpier | `framework/languages/dotnet/.config/dotnet-tools.json` | `.csharpierrc` `printWidth: 100`; `.csharpierignore`가 `csproj`·`props`·`targets`를 제외 |
| TypeScript | Prettier | `framework/languages/node/package.json` `devDependencies` | `.prettierrc` `printWidth: 100`, `singleQuote`, `trailingComma: none` |
| Java | google-java-format `--aosp` | `scripts/format/format.sh` `GJF_VERSION` | 없음(4칸 들여쓰기, 연속 8칸). import 정렬과 미사용 import 제거를 포함한다 |
| Kotlin | ktfmt `--kotlinlang-style` | `scripts/format/format.sh` `KTFMT_VERSION` | 없음(4칸 들여쓰기) |
| C++ | clang-format 18 | `scripts/format/format.sh` `CLANG_FORMAT_MAJOR` | 저장소 `.clang-format`(ColumnLimit 100)에 `quickstart/.clang-format`·`tutorial/.clang-format`·`samples/.clang-format`이 `BinPackArguments: false`와 `PenaltyBreakAssignment: 1000`을 더한다 |

빌드 스크립트(`csproj`, `*.kts`, CMake)는 포맷하지 않는다. `sync-version.py`와 빌드 도구의
관례가 그 파일의 모양을 정한다.

## 3. 실행

```bash
scripts/format/format.sh                 # 네 언어 전부 다시 쓴다
scripts/format/format.sh dotnet node     # 언어를 고른다
scripts/format/format.sh --check         # 바꿀 파일이 있으면 exit 1, 파일은 건드리지 않는다
```

- Java·Kotlin jar는 첫 실행에 Maven Central에서 `.artifacts/format/`으로 내려받는다.
- clang-format은 PATH의 `clang-format-18`(없으면 `clang-format`)을 쓰고, 주 버전이 다르면
  실행을 거부한다. Windows에서는 WSL에서 실행한다.
- Prettier는 `node_modules/.bin/prettier`가 있으면 그것을, 없으면 `package.json`의 버전으로
  `npx`를 쓴다.
- `--check`는 `scripts/gate/framework-gate.sh`가 첫 단계로 실행한다. 포맷 커밋은
  `.git-blame-ignore-revs`에 등록한다.

## 4. 이 규칙을 고른 근거

2026-09-20에 tutorial 소스로 다섯 포매터를 실측했다. 모두 규칙 1~3으로 수렴했고, 어느 것도
"100열 안에 들어가는 3개 이상 체인을 강제로 내려쓰기"를 지원하지 않았다. 그 규칙을 두면
손으로 내려쓴 줄을 포매터가 매번 되돌리므로 두지 않는다. 설정 항목이 많은 도구(ReSharper
Command Line Tools, Eclipse 포매터)로 닫는 `)` 위치나 연속 들여쓰기를 손 모양에 맞출 수도
있으나, 설정 파일을 유지하는 규칙이 하나 더 생기므로 설정 없는 도구의 출력을 규칙으로 삼았다.
