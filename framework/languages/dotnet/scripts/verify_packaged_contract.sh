#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DOTNET_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
REPO_ROOT="$(cd "$DOTNET_ROOT/../../.." && pwd)"
# 기계 판독용 계약 snapshot은 문서 트리가 아니라 .NET 코드 옆에 둔다.
SPEC_ROOT="$DOTNET_ROOT/contract"
SPEC_API_DIR="$SPEC_ROOT/api"
SPEC_PACKAGE_DIR="$SPEC_ROOT/packages"
VERSION="0.0.0-contract.$(date +%s).$$"
WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/zlink-dotnet-contract.XXXXXX")"
PACKAGE_DIR="$WORK_DIR/nuget"
CONSUMER_DIR="$WORK_DIR/consumer"
HTTP_CONSUMER_DIR="$WORK_DIR/http-consumer"
SOURCE_CONSUMER_DIR="$WORK_DIR/source-consumer"
INSPECTOR_DIR="$WORK_DIR/package-inspector"
SNAPSHOT_OUTPUT=""
if [[ "${1:-}" == "--generate-snapshot" && -n "${2:-}" && $# -eq 2 ]]; then
  SNAPSHOT_OUTPUT="$(realpath -m "$2")"
  if [[ "$SNAPSHOT_OUTPUT" == "$SPEC_ROOT" || "$SNAPSHOT_OUTPUT" == "$SPEC_ROOT"/* ]]; then
    echo "Snapshot generation must use a review directory outside the fixed spec tree." >&2
    exit 2
  fi
elif [[ $# -ne 0 ]]; then
  echo "usage: $0 [--generate-snapshot <review-output-directory>]" >&2
  exit 2
fi
trap 'rm -rf "$WORK_DIR"' EXIT

PROJECTS=(
  src/Zlink.Framework.Contracts/Zlink.Framework.Contracts.csproj
  src/Zlink.Framework.Provider.Abstractions/Zlink.Framework.Provider.Abstractions.csproj
  src/Zlink.Framework/Zlink.Framework.csproj
  src/Zlink.Framework.AspNetCore/Zlink.Framework.AspNetCore.csproj
  src/Zlink.Framework.Codecs.MessagePack/Zlink.Framework.Codecs.MessagePack.csproj
  src/Zlink.Framework.Codecs.Protobuf/Zlink.Framework.Codecs.Protobuf.csproj
  src/Zlink.Framework.Locations.Redis/Zlink.Framework.Locations.Redis.csproj
  src/Zlink.HttpClient/Zlink.HttpClient.csproj
  src/Systems.Zlink.Stream.Connector/Systems.Zlink.Stream.Connector.csproj
)
PACKAGE_IDS=(
  Zlink.Framework.Contracts
  Zlink.Framework.Provider.Abstractions
  Zlink.Framework
  Zlink.Framework.AspNetCore
  Zlink.Framework.Codecs.MessagePack
  Zlink.Framework.Codecs.Protobuf
  Zlink.Framework.Locations.Redis
  Zlink.HttpClient
  Systems.Zlink.Stream.Connector
)
OUT_OF_SCOPE_PACKABLE_PROJECTS=()

packable_projects=()
mapfile -t all_projects < <(rg --files "$DOTNET_ROOT" -g '*.csproj' | sort)
for project_path in "${all_projects[@]}"; do
  is_packable="$(dotnet msbuild "$project_path" \
    -nologo \
    -getProperty:IsPackable \
    -property:Configuration=Release)"
  case "$is_packable" in
    true)
      relative_project="${project_path#"$DOTNET_ROOT/"}"
      out_of_scope=0
      for excluded_project in "${OUT_OF_SCOPE_PACKABLE_PROJECTS[@]}"; do
        if [[ "$relative_project" == "$excluded_project" ]]; then
          out_of_scope=1
          break
        fi
      done
      if [[ "$out_of_scope" == "0" ]]; then
        packable_projects+=("$relative_project")
      fi
      ;;
    false)
      ;;
    *)
      printf 'Project %s evaluated IsPackable to unexpected value: %s\n' \
        "$project_path" "$is_packable" >&2
      exit 1
      ;;
  esac
done
mapfile -t packable_projects < <(printf '%s\n' "${packable_projects[@]}" | sort)
mapfile -t expected_projects < <(printf '%s\n' "${PROJECTS[@]}" | sort)
if [[ "$(printf '%s\n' "${packable_projects[@]}")" != "$(printf '%s\n' "${expected_projects[@]}")" ]]; then
  printf 'Packable project manifest differs from the frozen manifest.\nExpected:\n%s\nActual:\n%s\n' \
    "$(printf '%s\n' "${expected_projects[@]}")" \
    "$(printf '%s\n' "${packable_projects[@]}")" >&2
  exit 1
fi

mkdir -p "$PACKAGE_DIR" "$CONSUMER_DIR" "$HTTP_CONSUMER_DIR" "$SOURCE_CONSUMER_DIR" "$INSPECTOR_DIR"
for project in "${PROJECTS[@]}"; do
  dotnet pack "$DOTNET_ROOT/$project" \
    --configuration Release \
    --output "$PACKAGE_DIR" \
    --property:PackageVersion="$VERSION" \
    --property:ZLinkHttpClientDependencyVersion="$VERSION" \
    --nologo >/dev/null
done

mapfile -t packages < <(find "$PACKAGE_DIR" -maxdepth 1 -type f -name '*.nupkg' ! -name '*.symbols.nupkg' -printf '%f\n' | sort)
if [[ "${#packages[@]}" -ne "${#PACKAGE_IDS[@]}" ]]; then
  printf 'Expected %d packages, found %d:\n%s\n' "${#PACKAGE_IDS[@]}" "${#packages[@]}" "${packages[*]}" >&2
  exit 1
fi
for package_id in "${PACKAGE_IDS[@]}"; do
  package="$PACKAGE_DIR/$package_id.$VERSION.nupkg"
  [[ -f "$package" ]] || { echo "Missing package: $package" >&2; exit 1; }
  mapfile -t package_assemblies < <(unzip -Z1 "$package" | grep -E '^lib/net8\.0/[^/]+\.dll$' | sort)
  expected_assembly="lib/net8.0/$package_id.dll"
  if [[ "${#package_assemblies[@]}" -ne 1 || "${package_assemblies[0]:-}" != "$expected_assembly" ]]; then
    printf 'Package assembly manifest differs for %s. Expected %s, found: %s\n' \
      "$package_id" "$expected_assembly" "${package_assemblies[*]:-<none>}" >&2
    exit 1
  fi
  nuspec="$(unzip -p "$package" '*.nuspec')"
  grep -Fq "<id>$package_id</id>" <<<"$nuspec" || {
    echo "Package metadata has the wrong id: $package" >&2
    exit 1
  }
  grep -Fq "<version>$VERSION</version>" <<<"$nuspec" || {
    echo "Package metadata has the wrong version: $package" >&2
    exit 1
  }
  printf 'package=%s sha256=%s assembly=%s\n' \
    "$package_id" "$(sha256sum "$package" | cut -d' ' -f1)" "$expected_assembly"
done

cp "$SCRIPT_DIR/PackageContractSnapshot.cs" "$INSPECTOR_DIR/PackageContractSnapshot.cs"
cat >"$INSPECTOR_DIR/PackageInspector.csproj" <<'EOF'
<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <OutputType>Exe</OutputType>
    <TargetFramework>net8.0</TargetFramework>
    <ImplicitUsings>enable</ImplicitUsings>
    <Nullable>enable</Nullable>
  </PropertyGroup>
</Project>
EOF
cat >"$INSPECTOR_DIR/Program.cs" <<'EOF'
var version = args[0];
var outputDirectory = args[1];
foreach (var packagePath in args.Skip(2))
{
    var fileName = Path.GetFileName(packagePath);
    var marker = $".{version}.nupkg";
    if (!fileName.EndsWith(marker, StringComparison.Ordinal))
        throw new InvalidOperationException($"Unexpected package name: {fileName}");
    var packageId = fileName[..^marker.Length];
    File.WriteAllText(
        Path.Combine(outputDirectory, $"{packageId}.package.txt"),
        PackageContractSnapshot.Render(packagePath, version));
}
EOF
mkdir -p "$WORK_DIR/package-snapshots"
dotnet run --project "$INSPECTOR_DIR/PackageInspector.csproj" \
  --configuration Release -- \
  "$VERSION" "$WORK_DIR/package-snapshots" "$PACKAGE_DIR"/*.nupkg

framework_snapshot="$WORK_DIR/package-snapshots/Zlink.Framework.package.txt"
contracts_snapshot="$WORK_DIR/package-snapshots/Zlink.Framework.Contracts.package.txt"
redis_snapshot="$WORK_DIR/package-snapshots/Zlink.Framework.Locations.Redis.package.txt"
http_client_snapshot="$WORK_DIR/package-snapshots/Zlink.HttpClient.package.txt"
exact_connector_dependency="dependency targetFramework=net8.0 exclude=Build,Analyzers id=Systems.Zlink.Stream.Connector version=[{VERSION}]"
exact_framework_contracts_dependency="dependency targetFramework=net8.0 exclude=Build,Analyzers id=Zlink.Framework.Contracts version=[{VERSION}]"
exact_framework_provider_dependency="dependency targetFramework=net8.0 exclude=Build,Analyzers id=Zlink.Framework.Provider.Abstractions version=[{VERSION}]"
exact_redis_provider_dependency="dependency targetFramework=net8.0 exclude=Build,Analyzers id=Zlink.Framework.Provider.Abstractions version=[{VERSION}]"
exact_http_contracts_dependency="dependency targetFramework=net8.0 exclude=Build,Analyzers id=Zlink.Framework.Contracts version={VERSION}"
grep -Fxq "$exact_connector_dependency" "$framework_snapshot" || {
  echo "Zlink.Framework must pin the connector package to the exact framework package version." >&2
  exit 1
}
grep -Fxq "$exact_framework_contracts_dependency" "$framework_snapshot" || {
  echo "Zlink.Framework must pin the contracts package to the exact framework package version." >&2
  exit 1
}
grep -Fxq "$exact_framework_provider_dependency" "$framework_snapshot" || {
  echo "Zlink.Framework must pin the provider abstractions package to the exact framework package version." >&2
  exit 1
}
grep -Fxq "$exact_redis_provider_dependency" "$redis_snapshot" || {
  echo "Zlink.Framework.Locations.Redis must depend on the exact provider abstractions package." >&2
  exit 1
}
if grep -Fq "id=Zlink.Framework version=" "$redis_snapshot"; then
  echo "Zlink.Framework.Locations.Redis must not depend on the Framework runtime package." >&2
  exit 1
fi
grep -Fxq "$exact_http_contracts_dependency" "$http_client_snapshot" || {
  echo "Zlink.HttpClient must declare its Zlink.Framework.Contracts package dependency." >&2
  exit 1
}
for forbidden_dependency in \
  "Systems.Zlink.Stream.Connector" \
  "Systems.Zlink" \
  "K4os.Compression.LZ4"; do
  if grep -Fq "id=$forbidden_dependency version=" "$contracts_snapshot"; then
    echo "Zlink.Framework.Contracts must not depend on $forbidden_dependency." >&2
    exit 1
  fi
  if grep -Fq "id=$forbidden_dependency version=" "$http_client_snapshot"; then
    echo "Zlink.HttpClient must not depend on $forbidden_dependency." >&2
    exit 1
  fi
done
if grep -Fq "id=Zlink.Framework version=" "$http_client_snapshot"; then
  echo "Zlink.HttpClient must not depend on the Framework runtime package." >&2
  exit 1
fi

if [[ -n "$SNAPSHOT_OUTPUT" ]]; then
  mkdir -p "$SNAPSHOT_OUTPUT/packages"
  cp "$WORK_DIR/package-snapshots"/*.package.txt "$SNAPSHOT_OUTPUT/packages/"
else
  for package_id in "${PACKAGE_IDS[@]}"; do
    expected="$SPEC_PACKAGE_DIR/$package_id.package.txt"
    actual="$WORK_DIR/package-snapshots/$package_id.package.txt"
    [[ -f "$expected" ]] || {
      echo "Missing spec package snapshot: $expected" >&2
      exit 1
    }
    diff -u "$expected" "$actual"
  done
fi

cat >"$CONSUMER_DIR/NuGet.Config" <<EOF
<?xml version="1.0" encoding="utf-8"?>
<configuration>
  <packageSources>
    <clear />
    <add key="contract" value="$PACKAGE_DIR" />
    <add key="bindings" value="$REPO_ROOT/.artifacts/wsl/nuget" />
    <add key="nuget.org" value="https://api.nuget.org/v3/index.json" />
  </packageSources>
  <packageSourceMapping>
    <packageSource key="contract">
      <package pattern="Zlink.Framework" />
      <package pattern="Zlink.Framework.Contracts" />
      <package pattern="Zlink.Framework.Provider.Abstractions" />
      <package pattern="Zlink.Framework.AspNetCore" />
      <package pattern="Zlink.Framework.Codecs.MessagePack" />
      <package pattern="Zlink.Framework.Codecs.Protobuf" />
      <package pattern="Zlink.Framework.Locations.Redis" />
      <package pattern="Zlink.HttpClient" />
      <package pattern="Systems.Zlink.Stream.Connector" />
    </packageSource>
    <packageSource key="bindings">
      <package pattern="Systems.Zlink" />
    </packageSource>
    <packageSource key="nuget.org">
      <package pattern="Google.*" />
      <package pattern="K4os.*" />
      <package pattern="MessagePack*" />
      <package pattern="Microsoft.*" />
      <package pattern="Pipelines.*" />
      <package pattern="StackExchange.*" />
      <package pattern="System.*" />
    </packageSource>
  </packageSourceMapping>
</configuration>
EOF
cp "$CONSUMER_DIR/NuGet.Config" "$HTTP_CONSUMER_DIR/NuGet.Config"

cp "$SCRIPT_DIR/PublicContractSnapshot.cs" "$CONSUMER_DIR/PublicContractSnapshot.cs"
cp "$SCRIPT_DIR/PublicContractSnapshot.cs" "$SOURCE_CONSUMER_DIR/PublicContractSnapshot.cs"

cat >"$SOURCE_CONSUMER_DIR/SourceConsumer.csproj" <<EOF
<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <OutputType>Exe</OutputType>
    <TargetFramework>net8.0</TargetFramework>
    <ImplicitUsings>enable</ImplicitUsings>
    <Nullable>enable</Nullable>
  </PropertyGroup>
  <ItemGroup>
    <ProjectReference Include="$DOTNET_ROOT/src/Zlink.Framework/Zlink.Framework.csproj" />
    <ProjectReference Include="$DOTNET_ROOT/src/Zlink.Framework.Contracts/Zlink.Framework.Contracts.csproj" />
    <ProjectReference Include="$DOTNET_ROOT/src/Zlink.Framework.Provider.Abstractions/Zlink.Framework.Provider.Abstractions.csproj" />
    <ProjectReference Include="$DOTNET_ROOT/src/Zlink.Framework.AspNetCore/Zlink.Framework.AspNetCore.csproj" />
    <ProjectReference Include="$DOTNET_ROOT/src/Zlink.Framework.Codecs.MessagePack/Zlink.Framework.Codecs.MessagePack.csproj" />
    <ProjectReference Include="$DOTNET_ROOT/src/Zlink.Framework.Codecs.Protobuf/Zlink.Framework.Codecs.Protobuf.csproj" />
    <ProjectReference Include="$DOTNET_ROOT/src/Zlink.Framework.Locations.Redis/Zlink.Framework.Locations.Redis.csproj" />
    <ProjectReference Include="$DOTNET_ROOT/src/Zlink.HttpClient/Zlink.HttpClient.csproj" />
    <ProjectReference Include="$DOTNET_ROOT/src/Systems.Zlink.Stream.Connector/Systems.Zlink.Stream.Connector.csproj" />
  </ItemGroup>
</Project>
EOF

cat >"$SOURCE_CONSUMER_DIR/Program.cs" <<'EOF'
using Systems.Zlink.Stream.Connector.Contracts;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Codecs.MessagePack;
using Zlink.Framework.Codecs.Protobuf;
using Zlink.Framework.Contracts.Channels;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Locations.Redis;
using Zlink.Framework.LocationProvider;
using Zlink.HttpClient;

var assemblies = new[]
{
    typeof(IZLinkFrameworkOptions).Assembly,
    typeof(Zlink.Framework.Contracts.Codecs.ZLinkEncodedPayload).Assembly,
    typeof(IZLinkLocationStore).Assembly,
    typeof(ServiceCollectionExtensions).Assembly,
    typeof(ZLinkMessagePackCodec).Assembly,
    typeof(ZLinkProtobufCodec).Assembly,
    typeof(ZLinkRedisLocationStore).Assembly,
    typeof(ZLinkHttpClient).Assembly,
    typeof(IZlinkStreamConnector).Assembly
};
File.WriteAllText(args[0], PublicContractSnapshot.Render(assemblies));
EOF

cat >"$CONSUMER_DIR/Consumer.csproj" <<EOF
<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <OutputType>Exe</OutputType>
    <TargetFramework>net8.0</TargetFramework>
    <ImplicitUsings>enable</ImplicitUsings>
    <!-- The verifier pins each package to its isolated contract version below. -->
    <ManagePackageVersionsCentrally>false</ManagePackageVersionsCentrally>
    <Nullable>enable</Nullable>
  </PropertyGroup>
  <ItemGroup>
    <PackageReference Include="Zlink.Framework" Version="$VERSION" />
    <PackageReference Include="Zlink.Framework.Contracts" Version="$VERSION" />
    <PackageReference Include="Zlink.Framework.Provider.Abstractions" Version="$VERSION" />
    <PackageReference Include="Zlink.Framework.AspNetCore" Version="$VERSION" />
    <PackageReference Include="Zlink.Framework.Codecs.MessagePack" Version="$VERSION" />
    <PackageReference Include="Zlink.Framework.Codecs.Protobuf" Version="$VERSION" />
    <PackageReference Include="Zlink.Framework.Locations.Redis" Version="$VERSION" />
    <PackageReference Include="Zlink.HttpClient" Version="$VERSION" />
    <PackageReference Include="Systems.Zlink.Stream.Connector" Version="$VERSION" />
  </ItemGroup>
</Project>
EOF

cat >"$HTTP_CONSUMER_DIR/HttpConsumer.csproj" <<EOF
<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <OutputType>Exe</OutputType>
    <TargetFramework>net8.0</TargetFramework>
    <ImplicitUsings>enable</ImplicitUsings>
    <!-- Keep the standalone package probe independent of the repository's central pins. -->
    <ManagePackageVersionsCentrally>false</ManagePackageVersionsCentrally>
  </PropertyGroup>
  <ItemGroup>
    <PackageReference Include="Zlink.HttpClient" Version="$VERSION" />
  </ItemGroup>
</Project>
EOF

cat >"$HTTP_CONSUMER_DIR/Program.cs" <<'EOF'
using Zlink.HttpClient;

using var client = ZLinkHttpClient.Create("http://127.0.0.1:1").Build();
if (client.Get("/contract") is not ZLinkHttpRequestBuilder)
    throw new InvalidOperationException("The standalone HTTP client package did not load its public request surface.");
Console.WriteLine("dotnet standalone http package result=passed");
EOF

cat >"$CONSUMER_DIR/Program.cs" <<'EOF'
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Channels;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Contracts.Locations;
using Systems.Zlink;
using Systems.Zlink.Stream.Connector.Contracts;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Codecs.MessagePack;
using Zlink.Framework.Codecs.Protobuf;
using Zlink.Framework.Locations.Redis;
using Zlink.Framework.LocationProvider;
using Zlink.HttpClient;

var assembly = typeof(IZLinkRequestCall).Assembly;
var removedContracts = new[]
{
    "Zlink.Framework.Contracts.Channels.IZLinkYieldRequestCall",
    "Zlink.Framework.Contracts.Actors.IZLinkActorYieldJoinCall",
    "Zlink.Framework.Contracts.Locations.SpotRef",
    "Zlink.Framework.Contracts.Dispatch.ZLinkDispatchMode",
    "Zlink.Framework.Contracts.Assembly.ZLinkFrameworkAssemblyMarker",
    "Zlink.Framework.Contracts.Codecs.Json.ZLinkJsonCodecNamespace",
    "Zlink.Framework.Contracts.Handlers.ZLinkStreamRawAttribute",
    "Zlink.Framework.Contracts.Locations.IZLinkSpotRefResolver",
    "Zlink.Framework.Contracts.Locations.IZLinkActorAddressResolver",
    "Zlink.Framework.Contracts.Locations.IZLinkSpotHandleResolver",
    "Zlink.Framework.Contracts.Locations.IZLinkActorSpotHandleResolver",
    "Zlink.Framework.Contracts.Locations.SpotHandle",
    "Zlink.Framework.Contracts.Actors.IZLinkActorJoinCall"
}.Where(name => assembly.GetType(name) is not null).ToArray();
if (removedContracts.Length > 0)
    throw new InvalidOperationException(
        $"Removed public contracts are present in the package: {string.Join(", ", removedContracts)}");
if (!typeof(ActorRef).IsValueType
    || typeof(ActorRef).GetConstructor(
        [typeof(string), typeof(ulong), typeof(string), typeof(RoutingId)]) is null
    || typeof(IZLinkActorDeferredJoinCall).GetMethods().Select(method => method.Name).Order().SequenceEqual(new[] { "Defer" }) == false)
    throw new InvalidOperationException("The frozen public contract is missing from the package.");
var packagedAssemblies = new[]
{
    typeof(IZLinkFrameworkOptions).Assembly,
    typeof(Zlink.Framework.Contracts.Codecs.ZLinkEncodedPayload).Assembly,
    typeof(IZLinkLocationStore).Assembly,
    typeof(ServiceCollectionExtensions).Assembly,
    typeof(ZLinkMessagePackCodec).Assembly,
    typeof(ZLinkProtobufCodec).Assembly,
    typeof(ZLinkRedisLocationStore).Assembly,
    typeof(ZLinkHttpClient).Assembly,
    typeof(IZlinkStreamConnector).Assembly
};
if (packagedAssemblies.Select(static item => item.GetName().Name).Distinct(StringComparer.Ordinal).Count() != 9)
    throw new InvalidOperationException("Every framework contract package must load its own public assembly.");
if (typeof(ZLinkMessagePackCodec).GetProperty(nameof(ZLinkMessagePackCodec.Default)) is null
    || typeof(ZLinkProtobufCodec).GetProperty(nameof(ZLinkProtobufCodec.Default)) is null
    || typeof(ZLinkRedisLocationStore).GetConstructor([typeof(ZLinkRedisLocationOptions)]) is null
    || typeof(ZLinkHttpClient).GetMethod(nameof(ZLinkHttpClient.Create), Type.EmptyTypes) is null
    || typeof(ServiceCollectionExtensions).GetMethod(nameof(ServiceCollectionExtensions.AddZLinkFramework)) is null
    || typeof(ZlinkStreamConnectorFactory).GetMethod(nameof(ZlinkStreamConnectorFactory.Create)) is null)
    throw new InvalidOperationException("A supporting package public entry point is missing.");
File.WriteAllText(args[0], PublicContractSnapshot.Render(packagedAssemblies));
Console.WriteLine("dotnet packaged contract result=passed");
EOF

dotnet run --project "$SOURCE_CONSUMER_DIR/SourceConsumer.csproj" \
  --configuration Release -- "$WORK_DIR/source-api.txt"
NUGET_PACKAGES="$WORK_DIR/packages" dotnet run \
  --project "$CONSUMER_DIR/Consumer.csproj" \
  --configuration Release -- "$WORK_DIR/package-api.txt"
NUGET_PACKAGES="$WORK_DIR/http-packages" dotnet run \
  --project "$HTTP_CONSUMER_DIR/HttpConsumer.csproj" \
  --configuration Release

if [[ -n "$SNAPSHOT_OUTPUT" ]]; then
  mkdir -p "$SNAPSHOT_OUTPUT/api"
  rm -f "$SNAPSHOT_OUTPUT/api"/*.api.txt
  awk -v output="$SNAPSHOT_OUTPUT/api" '
    /^assembly / { file = output "/" $2 ".api.txt" }
    { if (file == "") exit 3; print > file }
  ' "$WORK_DIR/source-api.txt"
fi

if [[ -z "$SNAPSHOT_OUTPUT" ]]; then
  expected_api="$WORK_DIR/spec-api.txt"
  : >"$expected_api"
  mapfile -t sorted_package_ids < <(printf '%s\n' "${PACKAGE_IDS[@]}" | sort)
  for package_id in "${sorted_package_ids[@]}"; do
    snapshot="$SPEC_API_DIR/$package_id.api.txt"
    [[ -f "$snapshot" ]] || {
      echo "Missing spec public API snapshot: $snapshot" >&2
      exit 1
    }
    cat "$snapshot" >>"$expected_api"
  done
  if ! diff -u "$expected_api" "$WORK_DIR/source-api.txt" >"$WORK_DIR/spec-source-api.diff"; then
    echo "Source public API differs from the fixed spec snapshot:" >&2
    cat "$WORK_DIR/spec-source-api.diff" >&2
    exit 1
  fi
  if ! diff -u "$expected_api" "$WORK_DIR/package-api.txt" >"$WORK_DIR/spec-package-api.diff"; then
    echo "Packaged public API differs from the fixed spec snapshot:" >&2
    cat "$WORK_DIR/spec-package-api.diff" >&2
    exit 1
  fi
fi
if ! diff -u "$WORK_DIR/source-api.txt" "$WORK_DIR/package-api.txt" >"$WORK_DIR/public-api.diff"; then
  echo "Packaged public API differs from the validated source assemblies:" >&2
  cat "$WORK_DIR/public-api.diff" >&2
  exit 1
fi
assets="$CONSUMER_DIR/obj/project.assets.json"
[[ -f "$assets" ]] || { echo "Missing clean consumer assets file." >&2; exit 1; }
for package_id in "${PACKAGE_IDS[@]}"; do
  grep -Fq "\"$package_id/$VERSION\"" "$assets" || {
    echo "Clean consumer did not resolve $package_id/$VERSION." >&2
    exit 1
  }
  [[ -d "$WORK_DIR/packages/${package_id,,}/$VERSION" ]] || {
    echo "Clean consumer package cache is missing $package_id/$VERSION." >&2
    exit 1
  }
done
printf 'public_api_snapshot_sha256=%s\n' \
  "$(sha256sum "$WORK_DIR/package-api.txt" | cut -d' ' -f1)"
