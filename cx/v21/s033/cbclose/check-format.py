from pathlib import Path
import json
import subprocess
import sys

root = Path('/root/jobs/fx/cx/v21/s033/cbclose')
repo = Path('/root/worktree/zlink-4060-cpp-binding')
files = json.loads((root / 'touched-cpp-files.json').read_text())
results = {}
negative_cpp = root / 'format-negative.cpp'
negative_space = root / 'whitespace-negative.txt'
negative_cpp.write_text('int  main( ){return 0;}\n')
negative_space.write_text('trailing space \n')
with (root / 'format-and-whitespace.log').open('w') as log:
    commands = [
        ('clang-format', ['clang-format-18', '--dry-run', '--Werror', *files]),
        ('git-diff-check', ['git', 'diff', '--check']),
    ]
    commands += [(f'new-file-{Path(path).name}', ['git', 'diff', '--no-index', '--check', '/dev/null', path]) for path in files if '/tests/' in path]
    commands += [
        ('negative-clang-format', ['clang-format-18', '--style=file:' + str(repo / '.clang-format'), '--dry-run', '--Werror', str(negative_cpp)]),
        ('negative-git-diff-check', ['git', 'diff', '--no-index', '--check', '/dev/null', str(negative_space)]),
    ]
    for label, command in commands:
        result = subprocess.run(command, cwd=repo, capture_output=True, text=True)
        log.write(f'{label}: exit={result.returncode}\n{result.stdout}{result.stderr}')
        diagnostics = bool(result.stdout or result.stderr)
        if label.startswith('negative-'):
            ok = result.returncode != 0 and diagnostics
        elif label.startswith('new-file-'):
            ok = result.returncode in [0, 1] and not diagnostics
        else:
            ok = result.returncode == 0 and not diagnostics
        results[label] = {'exit': result.returncode, 'diagnostics': diagnostics, 'ok': ok}
(root / 'format-and-whitespace.json').write_text(json.dumps(results, indent=2) + '\n')
print(json.dumps(results))
negative_cpp.unlink()
negative_space.unlink()
sys.exit(any(not value['ok'] for value in results.values()))
