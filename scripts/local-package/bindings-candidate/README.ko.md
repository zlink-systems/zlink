# Python·Go·Rust bindings candidate package 검증

이 디렉터리는 아직 GitHub Release가 없는 Core 후보로 Python, Go 또는 Rust binding package 하나를 만들고
깨끗한 임시 consumer에서 설치·실행하는 비배포 진입점이다. 외부 registry 게시와 tag 생성은 수행하지
않는다.

먼저 공식 `core/build` runtime이 최신 source에서 만들어진 상태에서 manifest를 만든다.

```bash
scripts/local-package/bindings-candidate/create-manifest.sh /tmp/zlink-core-candidate.env
```

그 다음 언어를 하나씩 검증한다. RouteMesh 전환에서는 Python이 완전히 끝난 뒤 Go, Go가 완전히 끝난 뒤
Rust 순서로 실행한다.

```bash
scripts/local-package/bindings-candidate/build-wsl.sh \
  --language python \
  --manifest /tmp/zlink-core-candidate.env \
  --package-version 10.7.0
```

스크립트는 manifest의 Core revision, version, spec·header·source hash, raw export 목록, SONAME, raw public
struct layout, runtime hash와 freshness를 다시 계산해 비교한다. 동기화된 작업 tree뿐 아니라 실제
wheel·module archive·crate 안의 native payload와 header도 같은 후보인지 검사한다. 산출물 디렉터리의
`candidate-input.env`에는 이 provenance와 원본 manifest hash를 기록한다.

언어 package version의 major.minor가 Core base version과 다르거나 source tree를 우회하지 않은 consumer가
package를 사용할 수 없으면 실패한다. 언어 lane에서는 consumer의 최소 실행 코드를 새 MeshNode 공개
API와 엄격한 타입 또는 compile 검사까지 확장한다.
