[English](./bindings-node-1.2.1.md) | [한국어](./bindings-node-1.2.1.ko.md)

# ZLink Node.js binding 1.2.1 릴리스 노트

Core 1.2.0 위에서 동작하는 릴리스입니다. Core는 바뀌지 않았고 패키징만 고쳤습니다.

## 변경

- npm 패키지에 `prebuilds/win32-x64`를 함께 싣습니다. 1.2.0은 `linux-x64`만 담고 있어 Windows에서 `npm install`이 node-gyp 빌드로 넘어가 `ZLINK_CORE_INSTALL_PREFIX`를 요구했습니다. 이제 Windows x64에서는 네이티브 빌드 없이 설치됩니다. (#656)
- 소스 빌드에서 Core prefix에 `core-package-provenance.json`이 없어도 경고 뒤 진행합니다. 공개 Core 아카이브를 그대로 풀어 가리킨 경우입니다. header와 library 존재 검사는 그대로입니다. (#656)

## 검증

- 저장소 밖 빈 디렉터리에서 `ZLINK_*` 없이 tarball을 `npm install`했을 때 node-gyp가 실행되지 않고 `require('@zlink-systems/zlink')`가 동작했습니다.

릴리스 태그는 `node/v1.2.1`입니다.
