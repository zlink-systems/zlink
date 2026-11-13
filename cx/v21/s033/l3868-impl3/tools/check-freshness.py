import datetime, hashlib, json, pathlib, subprocess, sys
root = pathlib.Path('/root/worktree/zlink-v21-s033/l3868-next')
build = root / 'build-l3868-next'
# Ninja records every transitive source/header input of each compiled object.
records = []
for binary in ('bench_cpp_client', 'bench_cpp_zlink_server', 'bench_cpp_framework_server',
               'bench_cpp_grpc_server', 'bench_cpp_ladder_contract_test'):
    deps = subprocess.check_output(['ninja', '-C', str(build), '-t', 'inputs', binary], text=True)
    inputs = sorted({(build / line.strip()).resolve() for line in deps.splitlines() if line.strip()})
    inputs = [path for path in inputs if path.is_file()]
    newest = max(inputs, key=lambda path: path.stat().st_mtime_ns)
    artifact = build / binary
    record = dict(binary=str(artifact), sha256=hashlib.sha256(artifact.read_bytes()).hexdigest(),
                  input_count=len(inputs), newest_input=str(newest),
                  newest_input_mtime_ns=newest.stat().st_mtime_ns,
                  binary_mtime_ns=artifact.stat().st_mtime_ns,
                  fresh=artifact.stat().st_mtime_ns > newest.stat().st_mtime_ns)
    records.append(record)
result = dict(commit=subprocess.check_output(['git','rev-parse','HEAD'],cwd=root,text=True).strip(),
              dirty=subprocess.check_output(['git','status','--short'],cwd=root,text=True).splitlines(),
              utc=datetime.datetime.now(datetime.timezone.utc).isoformat(), artifacts=records)
pathlib.Path(sys.argv[1]).write_text(json.dumps(result, indent=2)+'\n')
for record in records:
    print(f"{pathlib.Path(record['binary']).name}: fresh={record['fresh']} inputs={record['input_count']} newest={record['newest_input']}")
assert all(record['fresh'] for record in records)
