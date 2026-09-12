[English](./README.md) | [한국어](./README.ko.md)

# Go local package

`build-wsl.sh` packages `bindings/go` as a source archive together with the current Core
`<VERSION>` runtime. The Go module path is `zlink.systems/zlink` and the release version is the
`<GO_BINDING_VERSION>` in `bindings/go/VERSION`.

```bash
scripts/local-package/go/build-wsl.sh \
  --core-prefix /absolute/path/.artifacts/wsl/install/zlink-core/<VERSION>
```

The output is this file.

```text
.artifacts/wsl/go/zlink-go-<GO_BINDING_VERSION>.tar.gz
```

The Go binding tests run before the build, and the archive carries the Linux x86_64 native payload
and `core-package-provenance.json` alongside the sources.
