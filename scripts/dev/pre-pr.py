"""Run the Linux x64 batch checks selected for a PR."""

import argparse
from contextlib import contextmanager
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = Path.cwd()
WORKFLOWS = Path(".github/workflows")
BASE_RANGE = "origin/main...HEAD"
LANGUAGES = ("cpp", "dotnet", "java", "node")
CHECKS = (
    "merge", "paths", "core", *LANGUAGES, "java-private-access", "jvm-api-snapshot",
    "dotnet-contract", "docs", "format",
)

# The step names select the Linux x64 main-queue checks. The workflows supply
# their commands, arguments, working directories, environment and order.
CPP = (
    "Verify framework relocation conformance inventory",
    "Resolve the runner vcpkg",
    "Install Framework C++ vcpkg dependencies",
    "Verify vcpkg versions against the manifest and Conan locks",
    "Build the checkout C++ binding package",
    "Configure and build Framework C++ tests",
    "Run Framework C++ unit and contract tests",
)
JAVA_TEST = "Run Framework Java core, integration and Spring starter tests"
JAVA = (
    "Verify framework relocation conformance inventory",
    "Fetch Core and build the checkout Java binding package on Linux",
    "Verify the packaged Java public contract and API snapshot",
    "Verify the packaged Kotlin public contract and API snapshot",
    JAVA_TEST,
    "Compile the Java and Kotlin samples",
    "Verify and summarize executed tests",
)
DOTNET_CONTRACT = "Verify the packaged public contract"
NODE_COMMON = (
    "Verify Node perf package.json pins",
    "Check the Linux native toolchain",
    "Prepare the Core release runtime for this platform",
    "Build the checkout Node binding package",
    "Build the local-package pin of @zlink-systems/http-client used by the workspace manifest",
    "Pin framework install to the checkout Node binding package",
    "Install framework dependencies",
    "Run Node framework release gate",
)
NODE_EXTRA = (
    "Run Node M6B runtime gate",
    "Run Node M6C runtime gate",
    "Verify packed Node packages in clean consumers",
)
NODE_BROWSER = "Run actual Chromium Stream Connector E2E"
NODE_DRIFT = "Verify the build left the committed WebGL bundle unchanged"
NODE_CROSS = (
    "Verify framework relocation conformance inventory",
    "Check the native toolchain",
    "Prepare the Core release runtime",
    "Prepare the .NET local packages for the cross-language test host",
    "Build the checkout Node binding package",
    "Build the local-package pin of @zlink-systems/http-client used by the workspace manifest",
    "Install and build the Node framework workspace",
    "Run Node framework cross-language smoke",
)
EXPRESSION = re.compile(r"\$\{\{\s*(.*?)\s*\}\}")


def command(*args, **kwargs):
    return subprocess.run(args, check=True, **kwargs).stdout


def git_output(*args):
    return command("git", *args, stdout=subprocess.PIPE)


def workflow_file(path):
    import yaml

    try:
        return yaml.load(path.read_text(encoding="utf-8"), Loader=yaml.BaseLoader)
    except yaml.YAMLError as error:
        raise ValueError(f"{path}: {error}") from error


def workflow(name):
    return workflow_file(WORKFLOWS / f"{name}.yml")


def path_matches(path, pattern):
    # GitHub's path wildcards: * stays in one component, ** crosses slashes.
    # Match **/ at the root too (for example "**/package-lock.json").
    tokens = {"**/": "(?:.*/)?", "**": ".*", "*": "[^/]*"}
    regex = "".join(tokens.get(token, re.escape(token))
                    for token in re.findall(r"\*\*/|\*\*|\*|[^*]+", pattern))
    return re.fullmatch(regex, path, flags=re.DOTALL) is not None


def workflow_matches(definition, files):
    for trigger in definition["on"].values():
        if not isinstance(trigger, dict):
            continue
        selected = set()
        for pattern in trigger.get("paths", ()):
            matched = {path for path in files if path_matches(path, pattern.removeprefix("!"))}
            if pattern.startswith("!"):
                selected.difference_update(matched)
            else:
                selected.update(matched)
        if selected:
            return True
    return False


def select():
    changes = git_output("diff", "--name-only", "--no-renames", "-z", BASE_RANGE)
    files = {os.fsdecode(path) for path in changes.split(b"\0") if path}
    # Merge-queue push triggers have no language paths. Use the language
    # selector from Ready for queue and the active main-cache dependency paths.
    env = dict(os.environ)
    env.pop("GITHUB_OUTPUT", None)
    selectors = command("bash", "scripts/ci/ready-for-queue-paths.sh", BASE_RANGE,
                        env=env, stdout=subprocess.PIPE, text=True).splitlines()
    selected = {name for name in LANGUAGES if f"{name}=true" in selectors
                or workflow_matches(workflow(f"framework-{name}-main-cache"), files)}
    if "core_version_changed=true" in selectors:
        selected.add("core")
    if workflow_matches(workflow("docs"), files):
        selected.add("docs")
    selected.add("format")  # The main queue requires this check on every batch.
    return selected


def redis_required():
    if not os.environ.get("ZLINK_REDIS_TEST_ENDPOINT", "").strip():
        raise ValueError("ZLINK_REDIS_TEST_ENDPOINT is required for Redis-backed tests")


def release_prefix():
    return command("bash", "scripts/local-package/core/fetch-release.sh",
                   "--platform", "linux-x64", stdout=subprocess.PIPE, text=True).strip()


def read_exports(path):
    return dict(line.split("=", 1) for line in path.read_text().splitlines())


class Job:
    """Call selected workflow steps with their Linux x64 working directory and inputs."""

    def __init__(self, root, temp, workflow_name, job_name, env=None, **context):
        definition = workflow(workflow_name)
        job = definition["jobs"][job_name]
        self.root = root
        self.temp = temp
        self.steps = job["steps"]
        self.outputs = {}
        self.outcomes = {}
        self.context = {
            "github.workspace": str(root), "github.token": os.environ.get("GH_TOKEN", ""),
            "inputs.single_worker": "false", "matrix.target.name": "linux-x64",
            "matrix.target.core_platform": "linux-x64",
            "matrix.target.local_package_root": ".artifacts/wsl", **context,
        }
        self.env = {
            **os.environ, "GITHUB_WORKSPACE": str(root), "RUNNER_OS": "Linux",
            "RUNNER_TEMP": str(temp),
            "ZLINK_CORE_CACHE_DIR": os.environ.get("ZLINK_CORE_CACHE_DIR",
                str(Path.home() / ".cache/zlink/core")),
        }
        for variables in (definition.get("env", {}), job.get("env", {})):
            self.env.update({key: self.resolve(value) for key, value in variables.items()
                             if key not in (env or {})})
        self.env.update(env or {})

    def value(self, name):
        if name.startswith("env."):
            return self.env[name.removeprefix("env.")]
        if name.startswith("steps."):
            _, step, _, key = name.split(".")
            return self.outputs[step][key]
        return self.context[name]

    def resolve(self, value):
        return EXPRESSION.sub(lambda match: self.value(match[1]), value)

    def run(self, *names):
        wanted = set(names)
        steps = [step for step in self.steps if step.get("name") in wanted]
        if {step["name"] for step in steps} != wanted:
            raise ValueError(f"Unknown workflow steps: {wanted - {step['name'] for step in steps}}")
        failed = False
        for step in steps:
            if failed and "always()" not in step.get("if", ""):
                continue
            try:
                self.run_step(step)
                self.outcomes[step["name"]] = True
            except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
                self.outcomes[step["name"]] = False
                if step.get("continue-on-error") == "true":
                    print(f"pre-pr: advisory: {error}", file=sys.stderr)
                    continue
                print(f"pre-pr: {error}", file=sys.stderr)
                failed = True
        return not failed

    def run_step(self, step):
        name = step["name"]
        print(f"pre-pr: running {name}", flush=True)
        if step.get("uses") == "./.github/actions/node-binding-package":
            action = workflow_file(self.root / ".github/actions/node-binding-package/action.yml")
            env = {"CORE_PREFIX": self.resolve(step["with"]["core-prefix"])}
            step = {**action["runs"]["steps"][0], "env": env}

        env = {**self.env, **{key: self.resolve(value) for key, value in step.get("env", {}).items()}}
        exports = {}
        for key in ("GITHUB_ENV", "GITHUB_OUTPUT", "GITHUB_PATH"):
            path = self.temp / key
            path.write_text("")
            env[key] = str(path)
            exports[key] = path

        shell = step.get("shell", "bash")
        source = step["run"]
        if shell == "pwsh":
            argv = ["pwsh", "-NoProfile", "-NonInteractive", "-Command", self.resolve(source)]
        else:
            def substitute(match):
                key = f"PRE_PR_INPUT_{len(env)}"
                env[key] = self.value(match[1])
                return "${" + key + "}"

            source = EXPRESSION.sub(substitute, source)
            argv = ["bash", "-e", "-o", "pipefail", "-c", source]

        directory = self.root / step.get("working-directory", ".")
        command(*argv, cwd=directory, env=env)
        self.env.update(read_exports(exports["GITHUB_ENV"]))
        paths = exports["GITHUB_PATH"].read_text().splitlines()
        if paths:
            self.env["PATH"] = os.pathsep.join([*reversed(paths), self.env["PATH"]])
        if "id" in step:
            self.outputs[step["id"]] = read_exports(exports["GITHUB_OUTPUT"])


def core(temp):
    return Job(ROOT, temp, "core-release-check", "check").run("Check published Core release")


def cpp(temp):
    redis_required()
    env = os.environ
    parallel = env.get("BUILD_PARALLEL") or "2"
    job = Job(ROOT, temp, "framework-cpp", "framework-cpp", env={
        "BUILD_PARALLEL": parallel,
        # The workflow reads the self-hosted parallelism from a repository variable,
        # which a local run has no access to; the local gate uses BUILD_PARALLEL.
        "BUILD_PARALLEL_SELF_HOSTED": parallel,
        "VCPKG_MAX_CONCURRENCY": env.get("VCPKG_MAX_CONCURRENCY") or parallel,
        "ZLINK_SCCACHE_LAUNCHER": env.get("ZLINK_SCCACHE_LAUNCHER", ""),
        "RUNNER_ARCH": "X64",
        "RUNNER_NAME": f"pre-pr-{ROOT.name}",
        "VCPKG_INSTALLATION_ROOT": env.get("VCPKG_INSTALLATION_ROOT") or env.get("VCPKG_ROOT", ""),
        **{key: env[key] for key in ("ZLINK_LOCAL_PACKAGE_ROOT",) if key in env},
    })
    # The local gate uses the Core release cache. CI rebuilds Core from source.
    prefix = Path(release_prefix())
    # fetch-release.sh publishes <cache>/release/<version>/<platform>/<digest> (#4817);
    # CMake reads the Core package at install/zlink-core/<version>.
    version = prefix.parents[1].name
    local_core = Path(job.env["ZLINK_LOCAL_PACKAGE_ROOT"]) / "install/zlink-core" / version
    # Preserve the release symlinks and refresh them when the local gate is rerun.
    local_core.mkdir(parents=True, exist_ok=True)
    command("cp", "-a", f"{prefix}/.", str(local_core))
    return job.run(*CPP)


def java(temp, selected, results):
    job = Job(ROOT, temp, "framework-java", "framework-java")
    if "java-private-access" in selected:
        results["java-private-access"] = verdict(job.run("Check Java test private access"))
    if "java" in selected:
        redis_required()
        if results["java-private-access"] == "fail":
            results["java"] = "fail"
            return
    if selected & {"java", "jvm-api-snapshot"} and not job.run(*JAVA[:2]):
        results["java" if "java" in selected else "jvm-api-snapshot"] = "fail"
        return
    if "jvm-api-snapshot" in selected:
        results["jvm-api-snapshot"] = verdict(job.run(*JAVA[2:4]))
    if "java" in selected:
        results["java"] = verdict(results["jvm-api-snapshot"] != "fail" and job.run(*JAVA[4:]))


def dotnet(temp, selected, results):
    if "dotnet" in selected:
        redis_required()
    workflow_name = "framework-dotnet"
    pack = Job(ROOT, temp, workflow_name, "prepare-dotnet-binding")
    if not pack.run("Pack the public .NET binding contract"):
        results["dotnet" if "dotnet" in selected else "dotnet-contract"] = "fail"
        return
    nuget = temp / "zlink-local-packages" / "nuget"
    shutil.copytree(temp / "zlink-dotnet-binding", nuget, dirs_exist_ok=True)
    job = Job(ROOT, temp, workflow_name, "framework-dotnet", **{
        "needs.prepare-dotnet-binding.outputs.artifact_name": pack.outputs["versions"]["artifact_name"]
    })
    if not job.run("Reject private reflection in .NET tests", "Prepare local dependency packages",
                   "Prepare the Core release runtime for this platform"):
        results["dotnet" if "dotnet" in selected else "dotnet-contract"] = "fail"
        return
    tests = [step["name"] for step in job.steps if step.get("run", "").startswith("bash scripts/ci/dotnet-test.sh ")]
    contracts = [name for name in tests if name.startswith("Run framework contract tests")]
    if not contracts:
        raise ValueError("No .NET contract test steps found in framework-dotnet.yml")
    build = ("Restore framework solution", "Build framework solution (Debug)",
             "Build framework solution (Release)")
    if not job.run(DOTNET_CONTRACT, *build):
        if job.outcomes.get(DOTNET_CONTRACT) is False or "dotnet" not in selected:
            results["dotnet-contract"] = "fail"
        if "dotnet" in selected:
            results["dotnet"] = "fail"
        return
    ok = job.run(*(tests if "dotnet" in selected else contracts))
    if all(job.outcomes.get(name) is True for name in contracts):
        results["dotnet-contract"] = "pass"
    elif any(job.outcomes.get(name) is False for name in contracts):
        results["dotnet-contract"] = "fail"
    if "dotnet" in selected:
        gate = Job(ROOT, temp, workflow_name, "framework-dotnet-gate")
        results["dotnet"] = verdict(ok and gate.run("Verify framework relocation conformance inventory"))


@contextmanager
def node_workspace(temp):
    # CI pins package.json/lock and regenerates a tracked WebGL bundle. Give each
    # matrix entry the equivalent of its own checkout without touching source files.
    root = temp / "node"
    root.mkdir()
    index = temp / "node-index"
    git_dir = os.fsdecode(git_output("rev-parse", "--absolute-git-dir")).strip()
    env = {**os.environ, "GIT_DIR": git_dir, "GIT_WORK_TREE": str(root),
           "GIT_INDEX_FILE": str(index)}
    command("git", "read-tree", "HEAD", env=env)
    command("git", "checkout-index", "--all", env=env, cwd=root)
    changes = git_output("diff", "HEAD", "--name-only", "--no-renames", "-z")
    for item in filter(None, changes.split(b"\0")):
        relative = Path(os.fsdecode(item))
        source = ROOT / relative
        destination = root / relative
        if destination.is_symlink() or destination.is_file():
            destination.unlink()
        if source.is_symlink() or source.is_file():
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, destination, follow_symlinks=False)
    try:
        yield root, {key: env[key] for key in ("GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE")}
    finally:
        shutil.rmtree(root)
        index.unlink()


def node(temp):
    redis_required()
    ok = True
    cross_env = None
    definition = workflow("framework-node")["jobs"]["framework-node"]
    versions = definition["strategy"]["matrix"]["node-version"]
    for version in versions:
        path = os.environ.get(f"PRE_PR_NODE{version}")
        path = os.pathsep.join((path, os.environ["PATH"])) if path else os.environ["PATH"]
        env = {"PATH": path, "NODE_OPTIONS": "--experimental-websocket" if version == "20" else ""}
        executable = shutil.which("node", path=path)
        if not executable or not command(executable, "--version", stdout=subprocess.PIPE,
                                         text=True).startswith(f"v{version}."):
            print(f"pre-pr: Node {version} is required (set PRE_PR_NODE{version} to its bin directory)", file=sys.stderr)
            ok = False
            continue
        with node_workspace(temp) as (root, git_env):
            env["PLAYWRIGHT_BROWSERS_PATH"] = os.environ.get(
                "PLAYWRIGHT_BROWSERS_PATH", str(ROOT / "framework/languages/node/.cache/ms-playwright"))
            job = Job(root, temp, "framework-node", "framework-node", env={**env, **git_env})
            if version == "22":
                cross_env = env
            matrix_ok = job.run(*NODE_COMMON, NODE_DRIFT, *(NODE_EXTRA if version == "22" else ()))
            if version == "22" and matrix_ok:
                # Require the SDK before the browser step, which can otherwise skip.
                sdk = Path(os.environ.get("ZLINK_EMSDK_ROOT", Path.home() / ".cache/zlink/emsdk"))
                if not (sdk / "upstream/emscripten/emcc").is_file():
                    print(f"pre-pr: emsdk 3.1.38 is required at {sdk}", file=sys.stderr)
                    matrix_ok = False
                else:
                    matrix_ok = job.run(NODE_BROWSER)
            ok = matrix_ok and ok

    if ok:
        with node_workspace(temp) as (root, git_env):
            job = Job(root, temp, "framework-node", "framework-node-cross-language", env={**cross_env, **git_env})
            ok = job.run(*NODE_CROSS)
    return ok


def docs(temp):
    job = Job(ROOT, temp, "docs", "build")
    return job.run(*(step["name"] for step in job.steps
                     if "run" in step and step["name"] != "Install dependencies"))


def format_check(temp):
    job = Job(ROOT, temp, "framework-format", "framework-format")
    return job.run("Check generated service-wire codecs", "Test entry join smoke harness",
                   "Check Framework SPDX licenses",
                   "Check source format")


def verdict(ok):
    return "pass" if ok else "fail"


def main():
    status = dict.fromkeys(CHECKS, "skip")
    try:
        parser = argparse.ArgumentParser(add_help=False, allow_abbrev=False)
        parser.add_argument("--no-merge", action="store_true")
        parser.add_argument("--only")
        args = parser.parse_args()
        requested = set(args.only.split(",")) if args.only is not None else set()
        valid = set(CHECKS) - {"merge", "paths"}
        if requested - valid:
            parser.error("unknown components: " + ", ".join(sorted(requested - valid)))
    except SystemExit:
        return 2

    try:
        if not args.no_merge:
            status["merge"] = "fail"
            if not git_output("branch", "--show-current").strip():
                raise ValueError("default merge requires a checked-out branch; use --no-merge on a detached HEAD")
            command("git", "fetch", "origin", "main")
            try:
                command("git", "merge", "--no-edit", "origin/main")
            except subprocess.CalledProcessError:
                command("git", "diff", "--name-only", "--diff-filter=U")
                raise
            status["merge"] = "pass"

        selected = requested
        if not selected:
            status["paths"] = "fail"
            selected = select()
            status["paths"] = "pass"
        if "java" in selected:
            selected.update(("java-private-access", "jvm-api-snapshot"))
        if "dotnet" in selected:
            selected.add("dotnet-contract")

        # Unattempted checks retain skip. A failed prerequisite is recorded by
        # the group that owns it, and independent checks still run.
        tools = {
            "core": core, "cpp": cpp,
            "dotnet": dotnet, "java": java, "node": node, "docs": docs, "format": format_check,
        }
        art = ROOT / ".artifacts"
        art.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="pre-pr-", dir=art) as temp:
            for name, run in tools.items():
                group = {
                    "java": {"java", "java-private-access", "jvm-api-snapshot"},
                    "dotnet": {"dotnet", "dotnet-contract"},
                }.get(name, {name}) & selected
                if not group:
                    continue
                try:
                    if name in ("dotnet", "java"):
                        run(Path(temp), group, status)
                    else:
                        status[name] = verdict(run(Path(temp)))
                except (ImportError, OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
                    print(f"pre-pr: {name}: {error}", file=sys.stderr)
                    failed_checks = {name} if name in group else group
                    for check in failed_checks:
                        status[check] = "fail"
    except (ImportError, OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        print(f"pre-pr: {error}", file=sys.stderr)
        return 1
    finally:
        print("pre-pr: " + " ".join(f"{name}={result}" for name, result in status.items()), flush=True)
    return int("fail" in status.values())


if __name__ == "__main__":
    sys.exit(main())
