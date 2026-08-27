#!/usr/bin/env python3
"""이동 커밋용 anchor 치환표를 만든다.

저장소 전체에서 옛 스펙 경로(`spec/server/NN-slug.{ko,en}.md#anchor`)를 찾아
새 경로의 anchor로 해석되는지 확인하고, 해석되지 않는 것을 제목 유사도로 매핑한다.
결과는 anchor-map.ko.md로 나간다. 문서를 고친 뒤 다시 돌려 표를 갱신한다.

사용: python3 doc/plan/spec-server-reorg/build-anchor-map.py
"""
import re, os, json, difflib, collections

ROOT = 'framework/doc/framework/common/spec/server'
PLAN = 'doc/plan/spec-server-reorg'
SKIP_DIRS = {'.git', 'node_modules', 'site', 'build', 'dist'}
EXTS = ('.md', '.json', '.sh', '.ts', '.java', '.cpp', '.cs', '.py', '.yml', '.hpp')


def slug(text):
    text = re.sub(r'\[([^\]]*)\]\([^)]*\)', r'\1', text.strip()).replace('`', '')
    s = re.sub(r'[^\w\s가-힣·-]', '', text.lower())
    return re.sub(r'[\s·]+', '-', s).strip('-')


def headings(path):
    out = []
    for line in open(path, encoding='utf-8'):
        m = re.match(r'^(#{2,4})\s+(.*)', line)
        if m:
            out.append((slug(m.group(2)), m.group(2).strip()))
    return out


def collect_anchors():
    """새 트리(주제 디렉터리)의 문서별 anchor 집합. 옛 평면 문서는 제외한다."""
    anchors = {}
    for d, _, fs in os.walk(ROOT):
        if '/languages' in d.replace('\\', '/'):
            continue
        for f in fs:
            if not f.endswith(('.ko.md', '.en.md')):
                continue
            rel = os.path.relpath(os.path.join(d, f), ROOT)
            if re.match(r'^\d\d-[a-z0-9-]+\.(ko|en)\.md$', rel):
                continue
            a = set()
            for line in open(os.path.join(d, f), encoding='utf-8'):
                m = re.match(r'^(#{2,4})\s+(.*)', line)
                if m:
                    a.add(slug(m.group(2)))
                a.update(re.findall(r'<a id="([^"]+)"', line))
            anchors[rel] = a
    return anchors


def load_move_map():
    """move-plan.ko.md의 1:1 이동 표만 읽는다. 1:N 행은 목적지가 여럿이라 제외한다."""
    mv = {}
    for line in open(f'{PLAN}/move-plan.ko.md', encoding='utf-8'):
        m = re.match(r'^\| `([0-9]{2}-[a-z0-9-]+)` \| `([^`]+)`', line)
        if m and '(' not in m.group(2):
            mv[m.group(1)] = m.group(2)
    return mv


def scan_references(mv, anchors):
    pat = re.compile(r'([\w./-]*?(\d\d-[a-z0-9-]+)\.(ko|en)\.md)(#([^)\s"\'`]+))?')
    ok = 0
    miss = collections.Counter()
    nomap = collections.Counter()
    for d, dirs, fs in os.walk('.'):
        dirs[:] = [x for x in dirs if x not in SKIP_DIRS]
        for f in fs:
            if not f.endswith(EXTS):
                continue
            p = os.path.join(d, f)[2:]
            if p.startswith('doc/site/site/'):
                continue
            try:
                text = open(p, encoding='utf-8', errors='ignore').read()
            except OSError:
                continue
            for m in pat.finditer(text):
                anchor, lang = m.group(5), m.group(3)
                if not anchor:
                    continue
                target = mv.get(m.group(2))
                if target is None:          # 1:N 문서 — 절 번호로 목적지를 골라야 한다
                    nomap[m.group(2)] += 1
                    continue
                key = f'{target}.{lang}.md'
                if key in anchors and anchor in anchors[key]:
                    ok += 1
                else:
                    miss[(m.group(2), lang, anchor)] += 1
    return ok, miss, nomap


def map_by_title(miss, mv):
    """옛 anchor의 절 제목을 찾아 새 문서에서 가장 비슷한 제목을 고른다."""
    strip_no = lambda x: re.sub(r'^\d+(\.\d+)*\.?\s*', '', x)
    rows, unresolved = [], []
    for (old, lang, anchor), count in miss.most_common():
        old_path = f'{ROOT}/{old}.{lang}.md'
        new_path = f'{ROOT}/{mv[old]}.{lang}.md'
        if not (os.path.exists(old_path) and os.path.exists(new_path)):
            unresolved.append(((old, lang, anchor, count), '파일 없음'))
            continue
        title = dict(headings(old_path)).get(anchor)
        if not title:
            unresolved.append(((old, lang, anchor, count), '옛 문서에 그 anchor 없음'))
            continue
        cand = sorted(
            (difflib.SequenceMatcher(None, strip_no(title), strip_no(t)).ratio(), s, t)
            for s, t in headings(new_path))
        score, new_anchor, new_title = cand[-1]
        if score >= 0.62:
            rows.append((old, lang, anchor, mv[old], new_anchor, round(score, 2), count))
        else:
            unresolved.append(((old, lang, anchor, count),
                               f'최고 유사도 {score:.2f} — {new_title}'))
    return rows, unresolved


def main():
    anchors = collect_anchors()
    mv = load_move_map()
    ok, miss, nomap = scan_references(mv, anchors)
    rows, unresolved = map_by_title(miss, mv)
    print(f'anchor 그대로 해석: {ok}')
    print(f'1:N 문서로 가는 참조: {sum(nomap.values())} (문서 {len(nomap)}개)')
    print(f'1:1인데 불일치: {sum(miss.values())} — 자동 매핑 {len(rows)}, 수동 {len(unresolved)}')


if __name__ == '__main__':
    main()
