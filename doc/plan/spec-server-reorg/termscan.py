"""용어집 용어의 첫 사용 자리를 점검한다.

가이드 §3.2 — 각 문서에서 용어집 용어를 처음 쓸 때 그 문맥에서 하는 일을 쉬운 문장으로
설명하고 용어집에 링크한다. 두 번째부터는 이름만 쓴다. 문서가 바뀌면 다시 설명한다.

두 가지를 잡는다.
  링크없음        — 그 문서가 용어를 쓰는데 용어집 링크가 하나도 없다
  첫등장이 링크보다 앞 — 문서 중간에 가서야 링크가 나온다(독자가 처음 만난 자리에서 모른다)

제목, front matter, 서두 blockquote, 내비게이션 줄, 문서 안 절 목차 표, 코드 fence는
첫 사용 판단에서 제외한다
(가이드 §3.2의 제목·목차 예외). 링크는 그 줄들에 있어도 인정한다.

    python3 doc/plan/spec-server-reorg/termscan.py '03-spot-actor/*.ko.md'
"""
import re, glob, sys, os

os.chdir('/home/hep7/project/zlink/framework/doc/framework/common/spec/server')
GLOSS = '00-foundation/02-glossary.ko.md'

def slug(t):
    a = t.lower()
    a = re.sub(r'[`*\[\]()]', '', a)
    a = re.sub(r'[^\w가-힣\s·\-]', '', a)
    return re.sub(r'\s+', '-', a).strip('-')

def load_terms():
    lines = open(GLOSS, encoding='utf-8').read().split('\n')
    terms = {}
    for i, l in enumerate(lines):
        m = re.match(r'^### (.+)$', l)
        if not m:
            continue
        t = m.group(1).strip()
        if len(t) < 4:
            continue
        anchors = {slug(t)}
        for j in (i - 1, i - 2, i + 1):
            if 0 <= j < len(lines):
                am = re.search(r'<a id="([^"]+)"', lines[j])
                if am:
                    anchors.add(am.group(1))
        terms[t] = anchors
    return terms

def scan(path, terms):
    raw = open(path, encoding='utf-8').read()
    lines = raw.split('\n')
    prose, infence = [], False
    for l in lines:
        if l.strip().startswith('```'):
            infence = not infence
            prose.append('')
            continue
        # 목차 표 행 — 셀 첫 칸이 그 문서 자신의 절 링크(#…)인 줄
        toc_row = bool(re.match(r'^\|\s*\[[^\]]+\]\(#[^)]+\)', l))
        if infence or toc_row or re.match(r'^(title:|---$|#{1,6} |>)', l) \
           or '주제 목차]' in l or '스펙 목차]' in l:
            prose.append('')                     # 줄 번호는 유지한다
            continue
        prose.append(l)
    out = []
    for t, anchors in terms.items():
        pat = re.compile(r'(?<![\w가-힣])' + re.escape(t) + r'(?![\w가-힣])')
        first = next((i for i, l in enumerate(prose) if pat.search(l)), None)
        if first is None:
            continue
        occ = sum(len(pat.findall(l)) for l in prose)
        linkpat = re.compile(r'\]\([^)]*02-glossary\.ko\.md#(' +
                             '|'.join(re.escape(a) for a in anchors) + r')\)')
        linkline = next((i for i, l in enumerate(lines) if linkpat.search(l)), None)
        if linkline is None:
            out.append((t, occ, '링크없음'))
        elif linkline > first:
            out.append((t, occ, f'첫등장 {first+1}행, 링크는 {linkline+1}행'))
    return out

terms = load_terms()
for pat in (sys.argv[1:] or ['*/*.ko.md']):
    for f in sorted(glob.glob(pat)):
        if f.endswith('02-glossary.ko.md'):
            continue
        rows = scan(f, terms)
        if rows:
            print(f"\n## {f}")
            for t, occ, why in sorted(rows, key=lambda x: -x[1]):
                print(f"  - {t} ({occ}회) — {why}")
