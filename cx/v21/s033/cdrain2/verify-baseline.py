from pathlib import Path
import hashlib,json,subprocess
repo=Path('/root/worktree/zlink-1639c');job=Path('/root/jobs/fx/cx/v21/s033/cdrain2');old=job.with_name('cdrain')
metadata=json.loads((old/'versions.json').read_text());baseline=metadata['baseline']
prefix='framework/languages/cpp/framework/src/runtime/'
entries=subprocess.check_output(['git','ls-tree','-r',baseline,'--',prefix],cwd=repo,text=True)
checked=0
for line in entries.splitlines():
 info,name=line.split('\t');expected=info.split()[2]
 p=old/'main-src'/name
 data=p.read_bytes();actual=hashlib.sha1(b'blob '+str(len(data)).encode()+b'\0'+data).hexdigest()
 assert actual==expected,(name,actual,expected)
 checked+=1
core=Path(metadata['core_library']);assert hashlib.sha256(core.read_bytes()).hexdigest()==metadata['core_sha256']
result={'baseline':baseline,'baseline_runtime_blobs_verified':checked,'core_library':str(core),'core_sha256':metadata['core_sha256'],'branch_head':subprocess.check_output(['git','rev-parse','HEAD'],cwd=repo,text=True).strip(),'baseline_benchmarks':{p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in (old/'main-build').glob('bench_cpp_*') if p.is_file() and p.suffix==''}}
(job/'versions.json').write_text(json.dumps(result,indent=2)+'\n');print(f'baseline runtime blobs verified={checked}; Core checksum unchanged; main benchmark checksums saved')
