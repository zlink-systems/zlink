#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "Usage: prepare-sample.sh SAMPLE_NAME DESTINATION" >&2
  exit 2
fi

sample_name="$1"
destination="$2"
case "$sample_name" in
  Bingo|DeliveryDispatch|GameQuest|ShoppingMall|SupportChat|TicTacToe|ZoneWorld) ;;
  *) echo "Unknown .NET sample: $sample_name" >&2; exit 2 ;;
esac

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(git -C "$script_dir" rev-parse --show-toplevel)"
samples_root="$repo_root/framework/languages/dotnet/samples"

copy_source_tree() {
  local source="$1"
  local target="$2"
  local file relative target_file
  while IFS= read -r -d '' file; do
    relative="${file#"$source"/}"
    target_file="$target/$relative"
    mkdir -p "$(dirname "$target_file")"
    cp -p "$file" "$target_file"
  done < <(find "$source" -type f ! -path '*/bin/*' ! -path '*/obj/*' -print0)
}

if [[ -e "$destination" && -n "$(find "$destination" -mindepth 1 -maxdepth 1 -print -quit 2>/dev/null)" ]]; then
  echo "Destination must not exist or must be empty: $destination" >&2
  exit 2
fi

mkdir -p "$destination"
cp -a "$samples_root/Directory.Build.props" "$destination/"
cp -a "$samples_root/Directory.Packages.props" "$destination/"
cp -a "$samples_root/nuget.config" "$destination/"
cp -a "$samples_root/redis-common.sh" "$destination/"
cp -a "$samples_root/sample_runner.ps1" "$destination/"
copy_source_tree "$samples_root/Common" "$destination/Common"
copy_source_tree "$samples_root/$sample_name" "$destination/$sample_name"

printf 'Prepared %s at %s\n' "$sample_name" "$(cd "$destination" && pwd)"
