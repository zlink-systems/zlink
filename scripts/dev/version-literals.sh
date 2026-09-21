#!/usr/bin/env bash
# Version numbers in scripts come from VERSION files (root VERSION, bindings/<lang>/VERSION,
# framework/languages/<lang>/VERSION) or from sync-version.py's target list; a literal such as
# 0.17.0 in a script goes stale at the next release and fails where nobody looks
# (rebuild-dev.sh did, #816). Lists every literal that looks like a zlink version on a
# non-comment line of scripts/**/*.{sh,ps1,py}; exit 1 when any is found.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
found=0
while IFS= read -r line; do
    found=1
    printf '%s\n' "$line"
done < <(grep -rnE --include='*.sh' --include='*.ps1' --include='*.py'     '(Zlink\.|zlink-systems-zlink-|libzlink\.so\.|zlink-core/|zlink-cpp/|zlink-c-|zlink-framework-|zlink-[0-9])[0-9]+\.[0-9]+\.[0-9]+' "$root/scripts"     | grep -vE '/tests/|/version-literals\.sh:'     | grep -vE '^[^:]*:[0-9]+:[[:space:]]*(#|//|<#|\*)'     | sed "s#^$root/##")
if ((found)); then
    echo "version literals in scripts/: read VERSION files instead (see scripts/dev/version-literals.sh)" >&2
    exit 1
fi
echo "ok: no version literals in scripts/"
