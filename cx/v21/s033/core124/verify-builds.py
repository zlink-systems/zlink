from pathlib import Path
import hashlib, subprocess
J=Path('/root/jobs/fx/cx/v21/s033/core124')
rows=[]
with (J/'ldd-all.txt').open('w') as log:
    for v in ('1.23','1.24'):
        root=J/f'src-{v}'; prefix=J/f'prefix-{v}'; build=root/'bindings/c' / 'build-release-1.23.0' / 'perf'
        exes=sorted(p for p in build.iterdir() if p.is_file() and p.stat().st_mode & 0o111)
        assert exes, f'no executables under {build}'
        for exe in exes:
            dynamic=subprocess.check_output(['readelf','-d',str(exe)],text=True)
            ldd=subprocess.check_output(['ldd',str(exe)],text=True)
            runpath=next((line.strip() for line in dynamic.splitlines() if '(RUNPATH)' in line or '(RPATH)' in line),'')
            core=next((line.strip() for line in ldd.splitlines() if 'libzlink.so' in line),'')
            if exe.name!='perf_stream_client':
                assert f'{prefix}/lib' in runpath,(exe,runpath)
                assert f'libzlink.so' in core and f'=> {prefix}/lib/' in core,(exe,core)
            else:
                assert not core,(exe,core)
            log.write(f'{exe}\n{runpath}\n{ldd}\n')
            rows.append([v,exe.name,hashlib.sha256(exe.read_bytes()).hexdigest(),runpath,core or 'no Core dependency'])
        assert subprocess.check_output(['git','-C',str(root),'status','--porcelain','--untracked-files=all'],text=True)==''
(J/'sha-ldd.tsv').write_text('version\texecutable\tsha256\trunpath\tcore_ldd\n'+''.join('\t'.join(r)+'\n' for r in rows))
print(f'PASS: {len(rows)} harness executables; each Core-linked executable resolves the matching prefix; source worktrees clean.')
