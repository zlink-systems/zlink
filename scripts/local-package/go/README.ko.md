# Go local package

`build-wsl.sh`는 `bindings/go`를 현재 Core `0.14.3` runtime과 함께 source
archive로 만든다. Go module path는 `zlink.systems/zlink`이고 release version은
`0.14.3`이다.

```bash
scripts/local-package/go/build-wsl.sh \
  --core-prefix /absolute/path/.artifacts/wsl/install/zlink-core/0.14.3
```

출력은 다음 파일이다.

```text
.artifacts/wsl/go/zlink-go-0.14.3.tar.gz
```

빌드 전에 Go binding test를 실행하며, archive에는 Linux x86_64 native
payload와 `core-package-provenance.json`을 함께 넣는다.
