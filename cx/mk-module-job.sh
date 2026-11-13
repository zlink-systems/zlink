#!/bin/bash
# usage: mk-module-job.sh <tag> <lang> <issue> <module-name-ko> <module-scope-ko> <branch> <gate>
set -e
TAG=$1; LANG=$2; ISSUE=$3; MOD=$4; SCOPE=$5; BR=$6; GATE=$7
J=/root/jobs/fx/cx/$TAG; W=/root/worktree/zlink-$TAG
git -C /root/worktree/zlink-main-base fetch -q
git -C /root/worktree/zlink-main-base worktree add -q -b $BR $W origin/main
mkdir -p $J
cat > $J/brief.md <<BRIEF
# 0.31.0 view 21 #$ISSUE — $LANG 직렬 왕복의 「$MOD」 모듈 비용 전부 제거

등급·레벨: sol medium, fast. 공통 규칙 \`/root/jobs/fx/cx/z301/common.md\`(그중 "시간 성능 측정 금지"는 아래 스모크 비교에는 적용하지 않는다). 문체는 \`doc/principal/documentation/documentation-principles.ko.md\` 원칙 7.
사용자 지시(2026-10-19·20): 진단에서 멈추지 말고 추천안을 구현한다. **출처 하나만 고치고 끝내지 않는다 — 이 모듈에서 찾은 Framework 소유 비용을 모두 고친다**(원인마다 커밋 분리, PR은 하나). 스펙 gap·공개 API·wire 변경이 필요할 때만 그 항목을 멈추고 나머지는 계속한다.

## 범위 — 「$MOD」
$SCOPE
- 이 모듈 밖의 파일은 고치지 않는다(다른 job이 같은 언어의 다른 모듈을 동시에 고친다). 범위 밖 발견은 file:line으로 보고만 한다.
- 이미 머지된 성능 PR(#2164 #2169 #2171 #2178 #2181 #2182 #2183 #2185 #2186 #2206 #2215 #2216 #2167)과 진행 중 PR(#2211 Node caller 결과, #1867 C++ 수신 역할 인계)과 겹치지 않게 최신 main에서 다시 센다.
- 앞 job 보고서가 있으면 먼저 읽는다: \`ls -d /root/jobs/fx/cx/k16*\` 중 같은 언어(k1640n*·k1675* Node, k1640d*·k1674* .NET, k1640j*·k1673* Java, k1672*·k1867* C++).

## 할 일
1. 1000회 직렬 typed request/reply fixture(앞 job 재사용)로 이 모듈의 왕복당 할당 바이트·객체 수·실행 자원 전환·대기·복사를 Framework 소유 stack별로 센다.
   - 수정 전/후 짧은 스모크 비교를 한다(사용자 지시 2026-10-20): 직렬 request-reply 1024B, warmup 2초·측정 5초, 3회 중앙값, Framework와 같은 회차의 binding을 함께 재서 Framework÷binding 비율 전/후를 표로 적는다. 측정 프로세스는 `taskset -c 12-15`로 고정한다(기존 bench runner: `/root/jobs/fx/cx/k1640b/run.py`·`adapter/`를 셀 하나로 줄여 쓴다). 스모크는 참고값이며, 판정은 계수 RED→GREEN과 하루 한 번 하는 전체 측정으로 한다.
2. 항목마다 스펙(\`framework/doc/framework/common/spec/server/01-execution/08-messaging-hot-path.ko.md\` I0–I4 :183-189, W1–W5 :219-228, §4.4 :280-309, \`01-submit-and-completion.ko.md\` §11 :397-427, \`04-application-job-queue-and-backpressure.ko.md\` §3·§4)으로 필요한지 판정하고, 필요하지 않은 것은 모두 없앤다.
3. 항목마다 결정적 RED→GREEN(왕복당 N회/바이트 기대값). 새 상태·pool·cache·ThreadLocal 금지, 판정 위치 증가 금지.
4. 다른 세 언어의 같은 단계를 file:line으로 대조한다(구현은 $LANG만).
5. gate: $GATE

## 보고서
\`$J/report.md\`(한국어): 항목 표(출처 / 왕복당 전→후 / 스펙 조항 / 제거·유지 이유), diff 요약, 판정 소유 표(전→후 판정 위치 수), RED·GREEN 로그, gate 수치와 로그 경로, 리뷰 8기준, 범위 밖 발견, "리팩토링 점검" 한 줄, 끝에 "## PR 본문 (한국어)". commit 본문에 스펙 file:line, \`Refs #$ISSUE\`, \`Refs #1640\`, 빈 줄 뒤 \`Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>\`.
$(cat /root/jobs/fx/cx/common-perf-tail.md)
worktree: $W, 브랜치 $BR(origin/main). 보고서: $J/report.md. **CPU 0-3은 성능 측정 전용이므로 절대 쓰지 않는다**: 모든 빌드·시험을 \`taskset -c 4-15\`로 실행한다. 이 머신에서 codex job 10개 이상이 동시에 돈다.
BRIEF
echo $J
