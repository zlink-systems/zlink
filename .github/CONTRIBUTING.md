# Contributing to zlink

zlink에 기여해 주셔서 감사합니다. 이 문서는 기여 프로세스를 설명합니다.

## 버그 리포트 / 기능 요청

- GitHub Issues에 등록해 주세요.
- 버그 리포트 시 재현 절차, OS/플랫폼, zlink 버전을 포함해 주세요.
- 보안 취약점은 Issues 대신 [SECURITY.md](../SECURITY.md) 절차를 따라 주세요.

## Pull Request

### 커밋 메시지

Conventional Commits 형식을 따릅니다:

```
feat: add new transport option
fix: resolve reconnect race condition
docs: update gateway guide
chore: bump version to 0.9.0
```

### PR 구성

- PR 하나당 하나의 명확한 변경사항을 다루세요.
- merge commit 대신 rebase를 사용해 주세요.
- PR 본문에 포함할 내용:
  - 변경 요약
  - 테스트 명령어 및 결과
  - 테스트한 플랫폼
  - 성능 민감 변경의 경우 벤치마크 결과

## 개발 환경 설정

### Linux

```bash
cd buildenv/linux
bash setup.sh --core    # 코어 빌드 도구
bash setup.sh --node    # Node.js 바인딩 (선택)
bash setup.sh --python  # Python 바인딩 (선택)
bash setup.sh --java    # Java 바인딩 (선택)
bash setup.sh --dotnet  # .NET 바인딩 (선택)
```

### Windows

```powershell
cd buildenv\win
.\setup.ps1 -Bindings core,node,python,java,dotnet
```

## 빌드 및 테스트

```bash
# CMake 빌드
cmake -B core/build -S core -DZLINK_BUILD_TESTS=ON
cmake --build core/build

# 테스트 실행
cd core/build && ctest --output-on-failure
```

또는 빌드 스크립트 사용:

```bash
./core/build.sh
```

## 코드 스타일

- `.clang-format` 규칙을 따릅니다: 4칸 들여쓰기, 탭 없음, 80컬럼 제한.
- 기존 `core/src/` 패턴과 일관성을 유지해 주세요.
- PR 제출 전 포매팅 확인:

```bash
clang-format --dry-run -Werror <changed-files>
```

## 테스트 작성

- 기능 테스트: `core/tests/test_*.cpp` (Unity 프레임워크)
- 내부 로직 테스트: `core/unittests/unittest_*.cpp`
- 플랫폼별 스킵이 있으면 PR에 명시해 주세요.

## 라이선스

이 프로젝트는 [MPL-2.0](../LICENSE) 라이선스입니다. 기여하신 코드는 동일 라이선스로 배포됩니다.
