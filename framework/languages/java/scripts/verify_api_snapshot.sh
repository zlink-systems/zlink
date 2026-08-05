#!/usr/bin/env bash
set -euo pipefail

language="${1:-}"
mode="${2:-verify}"
if [[ "$language" != "java" && "$language" != "kotlin" ]]; then
    echo "usage: $0 <java|kotlin> [verify|--candidate]" >&2
    exit 2
fi
if [[ "$mode" != "verify" && "$mode" != "--candidate" ]]; then
    echo "unknown mode: $mode" >&2
    exit 2
fi

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
repo_root="$(cd "$root_dir/../../.." && pwd)"
inventory="$repo_root/framework/doc/contract-inventory/jvm-public-contract-source-owners.json"
snapshot_rel="$(python3 - "$inventory" "$language" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as handle:
    inventory = json.load(handle)
print(inventory["apiSnapshots"][sys.argv[2]])
PY
)"
snapshot="$repo_root/$snapshot_rel"

work_dir="$(mktemp -d "${TMPDIR:-/tmp}/zlink-jvm-api-snapshot.XXXXXX")"
trap 'rm -rf "$work_dir"' EXIT
maven_dir="$work_dir/maven"
mkdir -p "$maven_dir"

mapfile -t owner_entries < <(python3 - "$inventory" "$language" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as handle:
    inventory = json.load(handle)

def public_owners(owner, artifact):
    for package in owner.get("publicOwners", []):
        print(f"{artifact}|{package}")

public_owners(inventory["serverArtifacts"]["java"], "zlink-framework-core")
for package in inventory["providerPublicOwners"]:
    print(f"zlink-framework-provider-abstractions|{package}")
public_owners(inventory["clientArtifacts"]["javaHttp"], "zlink-http-client")
public_owners(inventory["clientArtifacts"]["streamConnector"], "zlink-stream-connector")
for artifact, owner in zip(
    ("zlink-framework-codec-protobuf", "zlink-framework-codec-msgpack"),
    inventory["clientArtifacts"]["codecExtensions"]["publicOwners"],
):
    print(f"{artifact}|{owner}")

if sys.argv[2] == "kotlin":
    print(
        "zlink-framework-kotlin|"
        + inventory["serverArtifacts"]["kotlin"]["publicPackage"]
    )
    print(
        "zlink-http-client-kotlin|"
        + inventory["clientArtifacts"]["kotlinHttp"]["publicPackage"]
    )
PY
)

artifacts=()
for entry in "${owner_entries[@]}"; do
    artifacts+=("${entry%%|*}")
done
mapfile -t artifacts < <(printf '%s\n' "${artifacts[@]}" | sort -u)

publish_tasks=()
for artifact in "${artifacts[@]}"; do
    publish_tasks+=(":$artifact:publishAllPublicationsToReleaseRepoRepository")
done
MAVEN_REPOSITORY_URL="file://$maven_dir" \
    "$root_dir/gradlew" --no-daemon --no-parallel "${publish_tasks[@]}"

source_jars=()
package_jars=()
for artifact in "${artifacts[@]}"; do
    source_jar="$(find "$root_dir/$artifact/build/libs" -maxdepth 1 -type f \
        -name "$artifact-*.jar" ! -name '*-sources.jar' ! -name '*-javadoc.jar' \
        | sort -V | tail -n 1)"
    package_jar="$(find "$maven_dir/systems/zlink/$artifact" -type f \
        -name "$artifact-*.jar" ! -name '*-sources.jar' ! -name '*-javadoc.jar' \
        | sort -V | tail -n 1)"
    if [[ -z "$source_jar" || -z "$package_jar" ]]; then
        echo "api_snapshot_artifact_not_found artifact=$artifact" >&2
        exit 1
    fi
    source_jars+=("$source_jar")
    package_jars+=("$package_jar")
done

join_classpath() {
    local result=""
    local item
    for item in "$@"; do
        if [[ -z "$result" ]]; then
            result="$item"
        else
            result="$result:$item"
        fi
    done
    printf '%s' "$result"
}

render_surface() {
    local output="$1"
    local classpath="$2"
    local phase="$3"
    : > "$output"
    local entry artifact package package_path jar class_file class raw
    local -a classes
    local package_index=0
    for entry in "${owner_entries[@]}"; do
        artifact="${entry%%|*}"
        package="${entry#*|}"
        if [[ "$phase" == "source" ]]; then
            for jar in "${source_jars[@]}"; do
                [[ "$jar" == *"/$artifact-"* ]] || continue
                break
            done
        else
            for jar in "${package_jars[@]}"; do
                [[ "$jar" == *"/$artifact-"* ]] || continue
                break
            done
        fi
        package_path="$(printf '%s' "$package" | tr '.' '/')"
        printf 'artifact %s package %s\n' "$artifact" "$package" >> "$output"
        classes=()
        while IFS= read -r class_file; do
            class="${class_file%.class}"
            class="${class//\//.}"
            classes+=("$class")
        done < <(jar tf "$jar" \
            | awk -v package_path="$package_path" '
                $0 ~ "^" package_path "/[^/]+\\.class$" \
                    && $0 !~ /module-info\\.class$/ \
                    && $0 !~ /package-info\\.class$/ { print $0 }
            ' \
            | LC_ALL=C sort)
        if ((${#classes[@]} == 0)); then
            continue
        fi
        package_index=$((package_index + 1))
        raw="$work_dir/${phase}-${package_index}.javap"
        if ! javap -public -classpath "$classpath" "${classes[@]}" \
            > "$raw" 2>/dev/null; then
            echo "api_snapshot_javap_failed phase=$phase package=$package" >&2
            exit 1
        fi
        awk '
            /^Compiled from / {
                in_class = 1
                header = 1
                keep = 0
                next
            }
            in_class && header {
                header = 0
                keep = ($0 ~ /^public /)
                if (keep) print "class-header"
                if (keep) print $0
                next
            }
            in_class && keep {
                if ($0 !~ /^$/) print $0
                if ($0 == "}") in_class = 0
            }
        ' "$raw" >> "$output"
    done
}

source_classpath="$(join_classpath "${source_jars[@]}")"
package_classpath="$(join_classpath "${package_jars[@]}")"
source_surface="$work_dir/source.api.txt"
package_surface="$work_dir/package.api.txt"
render_surface "$source_surface" "$source_classpath" source
render_surface "$package_surface" "$package_classpath" package

if ! diff -u "$source_surface" "$package_surface" > "$work_dir/source-package.diff"; then
    echo "api_snapshot_source_package_drift language=$language" >&2
    cat "$work_dir/source-package.diff" >&2
    exit 1
fi

actual_sha="$(sha256sum "$package_surface" | awk '{print $1}')"
printf 'api_snapshot language=%s sha256=%s lines=%s\n' \
    "$language" "$actual_sha" "$(wc -l < "$package_surface")"

if [[ "$mode" == "--candidate" ]]; then
    exit 0
fi
if [[ ! -s "$snapshot" ]]; then
    echo "api_snapshot_not_found language=$language path=$snapshot_rel" >&2
    exit 1
fi
expected_sha="$(sed -n 's/^sha256=//p' "$snapshot" | head -n 1)"
if [[ -z "$expected_sha" ]]; then
    echo "api_snapshot_not_found language=$language path=$snapshot_rel" >&2
    exit 1
fi
if [[ "$actual_sha" != "$expected_sha" ]]; then
    echo "api_snapshot_drift language=$language expected=$expected_sha actual=$actual_sha" >&2
    exit 1
fi
echo "api snapshot verification passed: $language"
