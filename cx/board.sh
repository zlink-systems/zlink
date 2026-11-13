#!/usr/bin/env bash
# usage: board.sh <issue> <Todo|InProgress|Review|Done>
set -euo pipefail
issue=$1; status=$2
PROJECT=PVT_kwHOErOLTs4Bi-bl; FIELD=PVTSSF_lAHOErOLTs4Bi-blzhh0gcI
case $status in Todo) opt=c7603943;; InProgress) opt=fd26ca4e;; Review) opt=37fbabcf;; Done) opt=dff9b88d;; *) echo "bad status" >&2; exit 2;; esac
url=https://github.com/zlink-systems/zlink/issues/$issue
item=$(gh project item-add 1 --owner zlink-systems --url "$url" --format json --jq .id)
gh project item-edit --project-id $PROJECT --id "$item" --field-id $FIELD --single-select-option-id $opt >/dev/null
echo "#$issue -> $status"
