# framework 릴리스의 레지스트리 전파 대기 (2026-09-09)

## 결과

binding 0.17.5를 npm에 OIDC로 게시한 직후 framework 0.10.0 릴리스를 두 번 돌렸고
(run 34300160079, 34300342708) 둘 다 `npm ci`에서 실패했다. npm은 게시 직후 package
metadata는 보이지만 tarball(`/-/zlink-0.17.5.tgz`)은 몇 분 뒤에야 제공된다.
이번에는 3분 뒤 200으로 바뀌었다. framework 릴리스는 binding 릴리스 직후에 도는 것이
정상 순서이므로 이 지연은 워크플로우가 스스로 흡수해야 한다(사용자: "이건 gitaction에서
자동으로 되어야 하지 않을까?").

커밋 `0ed6a2331f`로 다음을 넣었다.

- `framework-release.yml` Node job과 Java job(Node workspace를 빌드해 interop 테스트를
  준비하는 단계) 앞에 "Wait for the binding package on npm" 단계. framework
  `package.json`의 `@zlink-systems/zlink` pin을 읽어 그 tarball URL이 200이 될 때까지
  1분 간격 최대 45분 기다린다. 넘기면 `::error::`로 실패한다.
- `release-dotnet.yml`의 framework job은 nuget.org flat container에서 `Zlink` nupkg를
  같은 방식으로 기다린 뒤 local source에 넣는다. nuget.org는 push 후 validation이 끝나야
  목록에 나타난다.

고친 커밋으로 돌린 run 34300580169는 Node·Java·C++ job이 모두 성공했고 Central
deployment `a284fbd3-dbf0-4b6c-b3a4-4f45a493fdcf`(AUTOMATIC, 13 purl)를 만들었다.
Central 공개 결과는 아래 "확인"에 적는다.

## 원칙

- 릴리스 순서는 bindings → framework이며 사람이 재실행하지 않는다. 전파 지연은 대기
  단계가, 실제 부재는 45분 뒤의 명확한 오류가 알려준다.
- 대기 대상 버전은 각 framework의 pin에서 읽는다. 버전 bump 시 워크플로우를 손대지
  않는다.
- 측정·검증 조건 완화가 아니다. 대기는 네트워크 전파에만 적용되고 테스트 timeout 등은
  그대로다.

## 확인

- Central deployment `a284fbd3-…`는 11:03에 `PUBLISHED`가 됐고(사람 클릭 없음), 13개
  artifact(`zlink-framework-*` 10개, `zlink-framework-kotlin`, `zlink-http-client`,
  `zlink-http-client-kotlin`, `zlink-stream-connector`)의 0.10.0 POM이
  `repo1.maven.org/maven2/systems/zlink/`에서 모두 200으로 확인됐다.
- Node framework 8개는 0.10.0이 이미 게시돼 있어 job이 건너뛰었다. C++ source archive는
  `framework/v0.10.0` Release에 재게시됐다.
- 이로써 framework 4언어(C++·Node·JVM(Java+Kotlin)·.NET)와 bindings 4언어(C++·Node·Java·.NET)
  0.17.5가 모두 공개 채널에 있다. .NET framework 9개는 앞서 run에서 nuget.org 9/9 노출을
  확인했다.
