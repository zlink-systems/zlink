[English](./README.md) | [한국어](./README.ko.md)

# 장르 템플릿 검증

`.github/workflows/framework-templates.yml`이 쓰는 스크립트다. 언어 5개(Java, Kotlin, .NET,
Node.js, C++)의 장르 템플릿마다 `./run-e2e.sh --local --build-only`와 Docker e2e
`./run-e2e.sh`를 실행한다.

| 파일 | 역할 |
|---|---|
| `plan.py` | `framework/languages/*/templates/`를 읽어 matrix를 만든다. 언어·템플릿 목록과 pull request의 변경 경로로 거른다 |
| `use-local-packages.py` | 템플릿 **사본**을 로컬에서 만든 Framework package에 연결한다 |
| `run-template.sh` | 사본 하나를 만들어 빌드와 e2e를 실행하고 결과 JSON을 남긴다 |
| `summary.py` | 결과를 언어·템플릿·빌드·e2e·시나리오 수 표로 만든다 |

템플릿은 공개 package만 쓴다([템플릿 제작 규칙](../../framework/templates/TEMPLATE-AUTHORING.ko.md)).
그래서 `source=local`은 템플릿 원본을 고치지 않고 실행 중 사본만 고친다. 사본 파일이 기대한
모양이 아니면 `use-local-packages.py`는 바로 실패한다.

## 실행

- pull request: 바뀐 템플릿만 공개 package로 실행한다. workflow나 이 디렉터리가 바뀌면 전부 실행한다.
- 수동 실행: `source`(public|local), `framework_version`(local일 때 필수), `languages`,
  `templates`(쉼표 목록, 비우면 전체)를 받는다. local은 체크아웃한 커밋의 Framework package를
  release workflow와 같은 명령으로 `framework_version`에 맞춰 만든다(커밋하지 않음).
- C++ job은 수동 실행에서만 `vars.ZLINK_CPP_LINUX_RUNNER`의 runner를 쓴다. pull request는 항상
  GitHub 호스팅 runner에서 돈다.

로컬에서 사본 하나를 연결하는 예:

```bash
cp -R framework/languages/node/templates/starter/my-game /tmp/node-starter
python3 scripts/templates/use-local-packages.py node /tmp/node-starter <packages-dir> 0.32.0 1.22.0
```
