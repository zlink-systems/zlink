#!/usr/bin/env bash
# Every SPDX header under framework/ must declare the license of its scope (#3928):
# Apache-2.0 for client connectors, engine adapters, http-client, samples, templates,
# tutorials and quickstarts; Elastic-2.0 for everything else (the server Framework).
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."

# Apache-2.0 scopes, as extended regular expressions over repository paths.
APACHE_SCOPES=(
  '^framework/languages/[^/]+/(samples|quickstart|tutorial|templates)/'
  '^framework/languages/shared_sample/'
  '/gradlew(\.bat)?$'
  '^framework/languages/cpp/common/'
  '^framework/languages/cpp/connector/(core|throwing-adapter)/'
  '^framework/languages/cpp/connector/engines/[^/]+/(include|src|Tools)/'
  '^framework/languages/cpp/connector/engines/unreal/Source/ZLinkStreamConnector/'
  '^framework/languages/cpp/http-client/'
  '^framework/languages/dotnet/(src|tests)/Zlink\.HttpClient'
  '^framework/languages/dotnet/src/Systems\.Zlink\.Stream\.Connector/'
  '^framework/languages/java/zlink-http-client'
  '^framework/languages/java/zlink-framework-json-internal/'
  '^framework/languages/node/packages/(http-client|stream-connector|stream-wire)/'
  '^framework/languages/node/test/(contract|browser)/http-'
)

# Apache-2.0 files outside the scopes above.
APACHE_FILES=(
  framework/languages/cpp/cmake/typed-json-allocations.cmake
  framework/languages/cpp/connector/tests/control_frame_contract.hpp
  framework/languages/cpp/connector/tests/test_connector_dispatch.cpp
  framework/languages/cpp/connector/tests/test_connector_unreal_fifo.cpp
  framework/languages/cpp/connector/tests/test_connector_wait_isolation.cpp
  framework/languages/cpp/framework/include/zlink/framework/codecs/json_stream_connector.hpp
  framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_typed_json_allocations.cpp
  framework/languages/cpp/tests/Zlink.Framework.UnitTests/typed_json_compile_fail.cpp
  framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/ZLinkJsonOptional.java
)

apache_pattern=$(IFS='|'; printf '%s' "${APACHE_SCOPES[*]}")
declare -A apache_file=()
for path in "${APACHE_FILES[@]}"; do apache_file["$path"]=1; done

failures=0
while IFS=: read -r path _ content; do
  [[ "$content" =~ SPDX-License-Identifier:[[:space:]]*([^[:space:]*/]+) ]] || continue
  actual=${BASH_REMATCH[1]}
  if [[ -v apache_file["$path"] || "$path" =~ $apache_pattern ]]; then expected=Apache-2.0; else expected=Elastic-2.0; fi
  if [[ "$actual" != "$expected" ]]; then
    printf '%s: expected %s, found %s\n' "$path" "$expected" "$actual"
    failures=$((failures + 1))
  fi
done < <(git grep -n -I 'SPDX-License-Identifier:' -- framework)

if (( failures > 0 )); then
  printf 'SPDX license check failed: %d file(s)\n' "$failures" >&2
  exit 1
fi
printf 'SPDX license check passed\n'
