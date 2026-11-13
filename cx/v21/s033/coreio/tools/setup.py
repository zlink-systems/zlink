from pathlib import Path
import hashlib,json,subprocess
R=Path('/root/worktree/zlink-ratio040'); J=Path('/root/jobs/fx/cx/v21/s033/coreio')
files=subprocess.check_output(['git','ls-files','-m','-o','--exclude-standard'],cwd=R,text=True).splitlines()
(J/'initial-state.json').write_text(json.dumps({'head':subprocess.check_output(['git','rev-parse','HEAD'],cwd=R,text=True).strip(),'branch':subprocess.check_output(['git','branch','--show-current'],cwd=R,text=True).strip(),'files':{f:hashlib.sha256((R/f).read_bytes()).hexdigest() for f in files}},indent=2))
s=(R/'framework/bench/grpc/runner_common.sh').read_text()
line='  trigger_phase "${trigger_url}" "${run_id}" "${cell_id}" "${pattern}" "${payload}" active "$((DURATION_SECONDS * 1000))"'
assert s.count(line)==1
s=s.replace(line,line+'\n  python3 /root/jobs/fx/cx/v21/s033/coreio/tools/collect.py --mode "${CAPTURE_MODE}" --source "${a_pid}" --target "${b_pid}" --source-url "${source_url}" --target-url "${target_url}" --out "${cell_dir}/capture"')
(J/'runner/runner_common.sh').write_text(s)
for lang in ('cpp','java'):
 s=(R/f'framework/bench/grpc/{lang}/run_local.sh').read_text()
 s=s.replace('HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"',f'HERE="{R}/framework/bench/grpc/{lang}"')
 s=s.replace('source "${HERE}/../runner_common.sh"',f'source "{J}/runner/runner_common.sh"')
 (J/f'runner/{lang}-run_local.sh').write_text(s)
