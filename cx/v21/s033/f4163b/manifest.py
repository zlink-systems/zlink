import pathlib,sys,json,hashlib,subprocess
p=pathlib.Path(sys.argv[1]); src=sys.argv[3]
runtime=next(f for f in (p/'lib').glob('libzlink.so.*') if f.is_file() and not f.is_symlink())
r=dict(schema=1,package='zlink-core',version='1.23.0',abiMajor=0,runtime=dict(path=str(runtime.relative_to(p)),sha256=hashlib.sha256(runtime.read_bytes()).hexdigest(),soname='libzlink.so.0'),source=dict(revision=subprocess.check_output(['git','-C',src,'rev-parse','HEAD'],text=True).strip(),dirty=bool(subprocess.check_output(['git','-C',src,'status','--porcelain','--untracked-files=no'],text=True).strip())),release=dict(tag='local/'+sys.argv[2],checksumsSha256=None),files=[])
(p/'share/zlink/core-package-provenance.json').write_text(json.dumps(r,indent=2)+'\n')
print(json.dumps(r))
