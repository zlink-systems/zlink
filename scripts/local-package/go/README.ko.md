# Go local package

`build-wsl.sh`는 승인된 Core candidate의 public header·runtime provenance를 확인한 뒤
Go binding을 표준 Go file proxy 형식으로 materialize하고, `replace`가 없는 별도 clean
consumer에서 compile·link·runtime message roundtrip을 검증한다. Package의 source는
현재 worktree 전체가 아니라 commit된 `HEAD`의 `bindings/go` snapshot에서 가져온다.
따라서 binding source에 commit하지 않은 변경이 있으면 package 생성이 실패한다.

현재 Go module은 `zlink.systems/zlink/v11@v11.1.0`이다. 공개 header와 native payload는
`bindings/go`에 포함된 Core 11 raw contract를 사용한다. `core/build`와 repository의
`core/include`는 package 입력으로 사용하지 않는다.

## 실행

Linux x86_64 payload를 사용해 local package와 clean consumer를 검증한다.

```bash
scripts/local-package/go/build-wsl.sh \
  --platforms linux-x86_64 \
  --core-candidate-manifest \
    /absolute/path/.artifacts/v11/evidence/V11-M3-CORE-VERIFY/candidate-reply-match-completion-hwm-20260801.json \
  --core-package-evidence \
    /absolute/path/.artifacts/v11/evidence/V11-M3-CORE-VERIFY/core-package-20260801.json \
  --output-root /absolute/path/.artifacts/wsl/go
```

`--core-candidate-manifest`와 `--core-package-evidence`는 같은 Core candidate를 가리켜야
한다. Builder는 candidate aggregate, V11-R2 review evidence, installed provenance,
runtime SHA-256과 Go header SHA-256을 확인한다. 현재 저장소에 같은 candidate evidence가
있는 platform은 Linux x86_64뿐이다.

Linux x86_64 이외의 platform을 추가하려면 같은 candidate identity의 native runtime과
package provenance를 먼저 준비해야 한다. 확인되지 않은 payload를 module zip에 넣으면
안 된다.

```bash
scripts/local-package/go/build-wsl.sh \
  --platforms linux-x86_64 \
  --core-candidate-manifest /absolute/path/candidate.json \
  --core-package-evidence /absolute/path/core-package.json \
  --output-root /absolute/path/.artifacts/wsl/go
```

지원하지 않는 platform이나 candidate와 hash가 다른 `bindings/go/native/<platform>/`를
지정하면 명령은 실패한다. 현재 worktree에서 package consumer가 실행된 platform과
결과는 Go 실행 계획의 검증 log에 기록한다.

## 산출물과 검증 범위

산출물은 다음 file proxy 경로를 따른다.

```text
<output>/proxy/zlink.systems/zlink/v11/@v/v11.1.0.info
<output>/proxy/zlink.systems/zlink/v11/@v/v11.1.0.mod
<output>/proxy/zlink.systems/zlink/v11/@v/v11.1.0.zip
<output>/go-package-v11.1.0.json
```

Module zip에는 commit된 Go source와 승인 candidate에 연결된 명령 지정 platform runtime만 포함한다.
`perf/build`, `perf/results`, 이전 service sample과 service header는 포함하지 않는다.
Clean consumer는 빈 `GOMODCACHE`와 `GOCACHE`에서 module을 `go mod download`하고,
`go build`한 뒤 module cache의 `libzlink.so.11`을 로드하여 Pair message roundtrip을
실행한다. Linux에서는 `ldd` 결과를 canonical path로 비교해 다른 repository runtime을
사용하지 않았는지 확인한다.

이 명령은 독립 Go POSD·DDD review를 대신하지 않는다. Candidate identity, review,
Core package provenance와 platform별 runtime hash는 Go 실행 계획의 package evidence에
함께 기록해야 한다.
