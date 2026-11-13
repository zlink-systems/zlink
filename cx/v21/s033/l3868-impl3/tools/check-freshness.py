import datetime, hashlib, json, pathlib, shlex, subprocess, sys
root = pathlib.Path('/root/worktree/zlink-v21-s033/l3868-next')
build = root / 'build-l3868-next'
# Ninja records every transitive source/header input of each compiled object.
records = []
recorded = {}
current = None
for line in subprocess.check_output(['ninja', '-C', str(build), '-t', 'deps'], text=True).splitlines():
    if ': #deps ' in line:
        current = line.split(': #deps ', 1)[0]
        recorded[current] = []
    elif line.startswith('    ') and current:
        recorded[current].append(line.strip())
binaries = sorted(path.name for path in build.glob('bench_cpp_*')
                  if path.is_file() and path.stat().st_mode & 0o111)
assert len(binaries) >= 11, binaries
for binary in binaries:
    deps = subprocess.check_output(['ninja', '-C', str(build), '-t', 'inputs', binary], text=True)
    inputs = {(build / line.strip()).resolve() for line in deps.splitlines() if line.strip()}
    commands = subprocess.check_output(['ninja', '-C', str(build), '-t', 'commands', binary], text=True)
    objects = set()
    for command in commands.splitlines():
        arguments = shlex.split(command)
        for index, argument in enumerate(arguments[:-1]):
            if argument == '-o' and arguments[index + 1].endswith('.o'):
                objects.add(arguments[index + 1])
    assert objects, binary
    for obj in objects:
        assert obj in recorded, f'missing header dependency record: {obj}'
        inputs.update((build / header).resolve() for header in recorded[obj])
    inputs = [path for path in sorted(inputs) if path.is_file()]
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
