#!/usr/bin/env python3
"""Check each template binding pin against the binding its published Framework requires.

framework/languages/<lang>/templates/TEMPLATE_VERSIONS pins the public Framework and binding
that the genre templates use. For every language that pins a binding, this reads the binding
that the Framework release requires at tag framework-<lang>/v<framework> and fails when the
pin differs. It needs the release tags (git fetch --tags); a missing tag is a failure.

scripts/local-package/sync-version.py keeps the templates equal to TEMPLATE_VERSIONS and
never reads tags, because it runs before every local-package build on shallow checkouts.
"""
from __future__ import annotations

import importlib.util
from pathlib import Path
import re
import subprocess
import sys

SYNC_VERSION = Path(__file__).resolve().parents[1] / "local-package" / "sync-version.py"
_spec = importlib.util.spec_from_file_location("sync_version", SYNC_VERSION)
sync_version = importlib.util.module_from_spec(_spec)
# dataclasses resolves annotations through sys.modules.
sys.modules[_spec.name] = sync_version
_spec.loader.exec_module(sync_version)

SEMVER = sync_version.SEMVER
SyncError = sync_version.SyncError

# Where each Framework release declares the binding it requires, read at its tag.
BINDING_SOURCES = {
    # The net8.0 nuspec dependency of Zlink.Framework, recorded by the package contract.
    "dotnet": (
        "framework/languages/dotnet/contract/packages/Zlink.Framework.package.txt",
        rf"id=Zlink version=({SEMVER})",
    ),
    "java": (
        "framework/languages/java/gradle/libs.versions.toml",
        rf'(?m)^zlinkBindings = "({SEMVER})"$',
    ),
    "node": (
        "framework/languages/node/packages/framework/package.json",
        rf'"@zlink-systems/zlink"\s*:\s*"({SEMVER})"',
    ),
}


def git(*arguments: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["git", "-C", str(sync_version.REPO_ROOT), *arguments],
        capture_output=True,
        encoding="utf-8",
        check=False,
    )


def tagged_file(tag: str, relative: str) -> str:
    if git("rev-parse", "--verify", "--quiet", f"refs/tags/{tag}^{{commit}}").returncode:
        raise SyncError(
            f"tag {tag} not found; fetch tags (git fetch --tags) to check the template "
            "binding pin against the published Framework"
        )
    shown = git("show", f"refs/tags/{tag}:{relative}")
    if shown.returncode:
        raise SyncError(f"cannot read {tag}:{relative}: {shown.stderr.strip()}")
    return shown.stdout


def check_template_bindings() -> dict[str, dict[str, str]]:
    """Return the checked pins; raise SyncError on the first mismatch or missing tag."""
    pins = sync_version.template_versions()
    for language in sync_version.TEMPLATE_BINDING_LANGUAGES:
        relative, pattern = BINDING_SOURCES[language]
        framework = pins[language]["framework"]
        binding = pins[language]["binding"]
        tag = f"framework-{language}/v{framework}"
        required = re.findall(pattern, tagged_file(tag, relative))
        if len(required) != 1:
            raise SyncError(
                f"{tag}:{relative}: expected 1 binding requirement, found {len(required)}"
            )
        if binding != required[0]:
            raise SyncError(
                f"{sync_version.template_versions_path(language)}: "
                f"{sync_version.TEMPLATE_BINDING_KEY}={binding}, but Framework {framework} "
                f"requires binding {required[0]} ({tag}:{relative})"
            )
    return pins


def main() -> int:
    try:
        pins = check_template_bindings()
    except (OSError, SyncError) as error:
        print(f"template binding check failed: {error}", file=sys.stderr)
        return 1
    checked = {language: pins[language] for language in sync_version.TEMPLATE_BINDING_LANGUAGES}
    print(f"Template binding pins {checked} match their Framework release tags")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
