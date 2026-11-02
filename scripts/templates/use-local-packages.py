#!/usr/bin/env python3
"""Points a copy of a genre template at locally built Framework packages.

Usage: use-local-packages.py <lang> <template-copy-dir> <packages-dir> <framework-version> <binding-version>

<lang> is java, kotlin, dotnet, node or cpp. <packages-dir> holds the layout the
packages jobs of framework-templates.yml upload:
  maven/  Maven file repository (Java, Kotlin)
  nuget/  *.nupkg (.NET)
  npm/    zlink-systems-<name>-<version>.tgz (Node)
  cpp/    zlink-framework-cpp-<version>-<platform>.tar.gz and .sha256 (C++)

Templates use only public packages (framework/templates/TEMPLATE-AUTHORING.ko.md), so this
edits files inside the given copy only and never the template source. Any file that does
not have the expected shape stops the script with an error.
"""
import json
import re
import shutil
import sys
from pathlib import Path


def fail(message):
    sys.exit(f"use-local-packages: {message}")


def read(path):
    with open(path, encoding="utf-8", newline="") as handle:
        return handle.read()


def write(path, text):
    with open(path, "w", encoding="utf-8", newline="") as handle:
        handle.write(text)


def edit(path, *rules):
    """Applies (pattern, replacement, required[, max_count]) regex rules to a file in place."""
    text = read(path)
    for pattern, replacement, required, *max_count in rules:
        text, count = re.subn(pattern, replacement, text, count=max_count[0] if max_count else 0, flags=re.M)
        if required and count == 0:
            fail(f"{path.name}: expected pattern not found: {pattern}")
    write(path, text)


def java(game, packages, fw, bind):
    shutil.copytree(packages / "maven", game / "local-maven")
    edit(game / "build.gradle.kts",
         (r'(extra\["frameworkVersion"\] = ")[^"]+', rf"\g<1>{fw}", True),
         (r'(extra\["bindingVersion"\] = ")[^"]+', rf"\g<1>{bind}", True))
    edit(game / "settings.gradle.kts",
         (r"repositories \{ mavenCentral\(\) \}",
          'repositories { maven { url = uri("local-maven") }; mavenCentral() }', True))


def dotnet(game, packages, fw, bind):
    nupkgs = sorted((packages / "nuget").glob("*.nupkg"))
    if not nupkgs:
        fail(f"no .nupkg in {packages / 'nuget'}")
    (game / "local-nuget").mkdir()
    for nupkg in nupkgs:
        shutil.copy(nupkg, game / "local-nuget")
    # Templates may pin the binding (Zlink) directly; Framework packages are Zlink.<Name>.
    edit(game / "Directory.Packages.props",
         (r'(Include="Zlink" Version=")[^"]+', rf"\g<1>{bind}", False),
         (r'(Include="Zlink\.[A-Za-z.]+" Version=")[^"]+', rf"\g<1>{fw}", True))
    edit(game / "nuget.config",
         (r"<clear />", '<clear />\n    <add key="local" value="local-nuget" />', True))
    edit(game / "Dockerfile",
         (r"^COPY Directory\.Build\.props Directory\.Packages\.props nuget\.config \./",
          r"\g<0>\nCOPY local-nuget local-nuget", True))


def node(game, packages, fw, bind):
    (game / "local-npm").mkdir()
    tgz = {}
    for archive in sorted((packages / "npm").iterdir()):
        match = re.match(r"zlink-systems-(.+)-(\d+\.\d+\.\d+)\.tgz$", archive.name)
        if match and match.group(2) == fw:
            shutil.copy(archive, game / "local-npm" / archive.name)
            tgz[f"@zlink-systems/{match.group(1)}"] = f"file:local-npm/{archive.name}"
    if not tgz:
        fail(f"no zlink-systems-*-{fw}.tgz in {packages / 'npm'}")
    manifest = game / "package.json"
    package = json.loads(read(manifest))
    for section in ("dependencies", "devDependencies"):
        for name in list(package.get(section, {})):
            if name == "@zlink-systems/zlink":
                package[section][name] = bind
            elif name in tgz:
                package[section][name] = tgz[name]
            elif name.startswith("@zlink-systems/"):
                fail(f"package.json: no local tarball for {name}")
    package["overrides"] = {**package.get("overrides", {}), **tgz, "@zlink-systems/zlink": bind}
    write(manifest, json.dumps(package, indent=2))
    (game / "package-lock.json").unlink(missing_ok=True)
    # The lockfile pins registry tarballs, so installs resolve again from package.json.
    edit(game / "run-e2e.sh", (r"npm ci", "npm install", False))
    edit(game / "Dockerfile",
         (r"npm ci", "npm install", False),
         (r"^(COPY package(?:-lock)?\.json[^\n]*\n)", r"COPY local-npm local-npm\n\1", True, 1),
         (r"COPY package\.json package-lock\.json \./", "COPY package.json ./", False))


def cpp(game, packages, fw, bind):
    archives = sorted((packages / "cpp").glob(f"zlink-framework-cpp-{fw}-*.tar.gz"))
    if len(archives) != 1 or not Path(f"{archives[0]}.sha256").is_file():
        fail(f"expected one zlink-framework-cpp-{fw}-<platform>.tar.gz with .sha256 in {packages / 'cpp'}")
    # bootstrap.cmake reads .zlink/downloads; .dockerignore drops .zlink, so the Docker
    # build gets the same files from local-prebuilt/.
    for target in (game / ".zlink" / "downloads", game / "local-prebuilt"):
        target.mkdir(parents=True)
        for source in (archives[0], Path(f"{archives[0]}.sha256")):
            shutil.copy(source, target)
    edit(game / "bootstrap.cmake",
         (r'(set\(ZLINK_FRAMEWORK_CPP_VERSION ")[^"]+', rf"\g<1>{fw}", True))
    edit(game / "Dockerfile",
         (r"^COPY bootstrap\.cmake \./$", r"\g<0>\nCOPY local-prebuilt/ .zlink/downloads/", True))


PREPARE = {"java": java, "kotlin": java, "dotnet": dotnet, "node": node, "cpp": cpp}


def main(argv):
    if len(argv) != 6 or argv[1] not in PREPARE:
        sys.exit(__doc__)
    lang, game, packages, fw, bind = argv[1], Path(argv[2]), Path(argv[3]), argv[4], argv[5]
    for name, value in (("framework", fw), ("binding", bind)):
        if not re.fullmatch(r"\d+\.\d+\.\d+", value):
            fail(f"invalid {name} version: {value}")
    if not (game / "run-e2e.sh").is_file():
        fail(f"{game} is not a template copy (no run-e2e.sh)")
    PREPARE[lang](game, packages, fw, bind)
    print(f"use-local-packages: {lang} {game} -> framework {fw}, binding {bind}")


if __name__ == "__main__":
    main(sys.argv)
