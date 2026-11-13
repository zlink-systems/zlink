
## 23:10 UTC — "Local HTTP client package was not found" 해결법 (감독자)
pre-pr.sh와 같은 절차를 각 worktree 루트에서 실행하십시오(push·GitHub 없이 로컬 build만):
1. `mkdir -p .artifacts/wsl/npm && cp /root/.cache/zlink/packages/local-1.25.0/node/npm/zlink-systems-zlink-1.25.0.tgz .artifacts/wsl/npm/`
2. `unset ZLINK_LOCAL_PACKAGE_ROOT ZLinkLocalPackageRoot; bash scripts/local-package/http-client/build-wsl.sh node`
3. 그 다음 `cd framework/languages/node && npm ci` 후 원래 build를 계속합니다.
Core binding version은 bindings/node/VERSION의 ZLINK_BINDING_VERSION(1.25.0)입니다.
