#!/usr/bin/env python3
"""Plans the Framework genre-template matrix for framework-templates.yml.

Usage: plan.py <languages> <templates> [<changed-paths-file>]

<languages> and <templates> are comma lists; empty means all. With a changed-paths
file (pull_request), only templates with a changed file run, unless the workflow or
scripts/templates changed, in which case every template runs.

Templates are discovered from the tree: framework/languages/<package>/templates/<name>/my-game
and, for languages that share one package (Java and Kotlin),
framework/languages/<package>/templates/<lang>/<name>/my-game.

Prints one JSON object: {"matrix": {"include": [...]}, "cpp_matrix": {"include": [...]},
"count": N, "cpp_count": N, "packages": [...]} where each entry is
{lang, template, path, package}. package is the framework language directory that owns
the packages and the binding VERSION (java for kotlin).
"""
import json
import sys
from pathlib import Path

# Changes here re-run every template.
PLAN_INPUTS = (".github/workflows/framework-templates.yml", "scripts/templates/")
# C++ runs in its own job (own runner and parallelism); everything else shares one job.
CPP = "cpp"


def discover(root):
    entries = []
    for game in sorted(root.glob("framework/languages/*/templates/*/my-game")):
        entries.append((game.parts[-4], game.parts[-2], game))
    for game in sorted(root.glob("framework/languages/*/templates/*/*/my-game")):
        entries.append((game.parts[-3], game.parts[-2], game))
    result = []
    for lang, template, game in entries:
        rel = game.relative_to(root).as_posix()
        result.append({"lang": lang, "template": template, "path": rel, "package": rel.split("/")[2]})
    if not result:
        sys.exit("no templates found under framework/languages/*/templates")
    return sorted(result, key=lambda e: (e["lang"], e["template"]))


def select(entries, key, requested):
    names = [n.strip() for n in requested.split(",") if n.strip()]
    if not names:
        return entries
    known = {e[key] for e in entries}
    unknown = sorted(set(names) - known)
    if unknown:
        sys.exit(f"unknown {key}: {', '.join(unknown)} (known: {', '.join(sorted(known))})")
    return [e for e in entries if e[key] in names]


def changed_only(entries, changed):
    if any(p == PLAN_INPUTS[0] or p.startswith(PLAN_INPUTS[1]) for p in changed):
        return entries
    # A template owns its whole directory (the parent of my-game).
    return [e for e in entries if any(p.startswith(e["path"].rsplit("/", 1)[0] + "/") for p in changed)]


def main(argv):
    if len(argv) not in (3, 4):
        sys.exit(__doc__)
    entries = discover(Path.cwd())
    entries = select(entries, "lang", argv[1])
    entries = select(entries, "template", argv[2])
    if len(argv) == 4:
        changed = [line.strip() for line in Path(argv[3]).read_text().splitlines() if line.strip()]
        entries = changed_only(entries, changed)
    other = [e for e in entries if e["package"] != CPP]
    cpp = [e for e in entries if e["package"] == CPP]
    print(json.dumps({
        "matrix": {"include": other},
        "cpp_matrix": {"include": cpp},
        "count": len(other),
        "cpp_count": len(cpp),
        "packages": sorted({e["package"] for e in other}),
    }, separators=(",", ":")))


if __name__ == "__main__":
    main(sys.argv)
