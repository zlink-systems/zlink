# ABI soversion 다루기

Core 릴리스에서 링커 SONAME을 정하는 방법이다. 번호 규칙 자체는
[`versioning.ko.md`](../versioning.ko.md) §2가 소유한다.

## 소유자

root `VERSION`의 `LIBZLINK_ABI_SOVERSION` 한 줄이 SONAME major를 소유한다.

```
LIBZLINK_VERSION=1.0.0
LIBZLINK_ABI_SOVERSION=0
```

이 값은 패키지 버전과 무관하다. 공개 C API(`core/include/**`, `core/src/libzlink.vers`)가 바뀌지
않으면 패키지 `MAJOR`가 올라도 그대로 둔다. Core 1.0.0의 라이브러리가 `libzlink.so.0`인 것이
그래서다.

## 읽는 쪽

다음이 모두 `VERSION`에서 읽는다. 값을 복제하거나 패키지 버전에서 뽑지 않는다.

| 위치 | 쓰임 |
| --- | --- |
| `core/CMakeLists.txt` | `set_target_properties(... SOVERSION ...)` |
| `core/builds/linux/build.sh` | 아카이브에 넣을 `libzlink.so.<soversion>` 링크 |
| `.github/workflows/bindings-release.yml` | `resolve` job이 한 번 읽어 output으로 넘기고 node·go·cpp·rust·java job이 사용 |
| `scripts/local-package/sync-version.py` | 필드 검증 |

`scripts/local-package/**`의 여러 스크립트는 아직 `libzlink.so.0`과 `abiMajor: 0`을 직접 적는다.
현재 값과 같아 동작하지만 soversion을 올릴 때 함께 고쳐야 한다. 같은 값을 읽도록 옮기는 일은
`#344`에 남아 있다.

## ABI를 깰 때

1. `VERSION`의 `LIBZLINK_ABI_SOVERSION`을 올린다. **그것만 올린다.**
2. `python3 scripts/local-package/sync-version.py --write`
3. 위 표의 소비자가 새 값을 쓰는지 확인한다.

```bash
# 패키지 버전에서 soversion을 뽑는 코드가 생기지 않았는지
rg -n '%%\.\*' core/builds .github/workflows scripts/

# 빌드 산출물의 실제 SONAME
readelf -d core/dist/linux-x64/libzlink.so | grep SONAME
```

## 릴리스 아카이브 점검

Core 릴리스 아카이브의 linux 항목은 다음 둘이어야 한다.

- `libzlink.so` — 실제 라이브러리
- `libzlink.so.<soversion>` — 그것을 가리키는 심볼릭 링크

링커는 ELF SONAME으로 라이브러리를 찾으므로, `readelf -d`가 내놓는 이름과 같은 이름의 파일이
아카이브에 반드시 있어야 한다. 그 이름이 없으면 아카이브를 직접 쓰는 소비자(C++ binding, vcpkg,
Conan)가 링크 후 실행에서 실패한다.

```bash
tar tzf libzlink-linux-x64.tar.gz | grep 'libzlink\.so'
```
