#!/usr/bin/env bash
# 내부 운영 문서(비공개 zlink-systems/docs)를 준비하고 내부 AGENTS.md 경로를 출력한다(#2203).
# 위치는 zlink 기본 사본의 옆 폴더 zlink-docs다. worktree에서 실행해도 같은 위치가 나온다.
# 받을 권한이 없으면 메시지를 내고 0으로 끝난다. 그때는 공개 규칙만 따른다.
set -euo pipefail
common_dir=$(git rev-parse --path-format=absolute --git-common-dir)
docs_dir="$(dirname "$(dirname "$common_dir")")/zlink-docs"
if [ -d "$docs_dir/.git" ]; then
    git -C "$docs_dir" pull -q --ff-only || echo "internal-docs: pull 실패, 기존 사본을 쓴다: $docs_dir" >&2
elif ! gh repo clone zlink-systems/docs "$docs_dir" -- -q 2>/dev/null; then
    echo "internal-docs: zlink-systems/docs를 받을 수 없다. 공개 규칙만 따른다." >&2
    exit 0
fi
agents_dir="$HOME/.claude/agents"
mkdir -p "$agents_dir"
cp "$docs_dir/zlink/.claude/agents/"*.md "$agents_dir/"
echo "$docs_dir/zlink/AGENTS.md"
