from pathlib import Path
import importlib.util, json, sys
J=Path('/root/jobs/fx/cx/v21/s033/core124')
spec=importlib.util.spec_from_file_location('gate',J/'src-1.24/bindings/c/perf/perf_regression_gate.py')
gate=importlib.util.module_from_spec(spec);sys.modules[spec.name]=gate;spec.loader.exec_module(gate)
rows=[]; groups={}
for suite in ('single','multi'):
    baseline=gate.parse_report(J/f'results/B1-{suite}.txt')
    candidate=gate.parse_report(J/f'results/C-{suite}.txt')
    errors=list(gate.report_errors(f'{suite} baseline',baseline))+list(gate.report_errors(f'{suite} candidate',candidate))
    if errors: raise SystemExit('\n'.join(errors))
    cells=gate.compare_reports(suite,baseline,candidate)
    aggs=gate.compare_aggregates(cells)
    for v in cells:
        rows.append({'suite':suite,'kind':'cell','pattern':v.key[0],'transport':v.key[1],'size':v.key[2],'metric':v.key[3],'ratio':v.ratio,'status':v.status})
        if not v.status.startswith('PASS'): groups.setdefault((suite,v.key[0],v.key[1]),set()).add('failed cell')
    for v in aggs:
        rows.append({'suite':suite,'kind':'aggregate','pattern':v.pattern,'transport':v.transport,'metric':v.metric,'geometric_mean':v.geometric_mean,'status':v.status})
        if not v.status.startswith('PASS'): groups.setdefault((suite,v.pattern,v.transport),set()).add('failed aggregate')
(J/'gate-failures.json').write_text(json.dumps(rows,indent=2)+'\n')
path=J/'confirm/groups.tsv'
path.parent.mkdir(exist_ok=True)
path.write_text(''.join(f'{s}\t{p}\t{t}\t{", ".join(sorted(reasons))}\n' for (s,p,t),reasons in sorted(groups.items())))
print(f'groups={len(groups)} cells={sum(r["kind"]=="cell" for r in rows)} failed_cells={sum(r["kind"]=="cell" and not r["status"].startswith("PASS") for r in rows)} failed_aggregates={sum(r["kind"]=="aggregate" and not r["status"].startswith("PASS") for r in rows)}')
