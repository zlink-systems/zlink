[English](./README.md) | [한국어](./README.ko.md)

# Genre template checks

Scripts used by `.github/workflows/framework-templates.yml`. For every genre template of the five
languages (Java, Kotlin, .NET, Node.js, C++) it runs `./run-e2e.sh --local --build-only` and the
Docker e2e `./run-e2e.sh`.

| File | Role |
|---|---|
| `plan.py` | Builds the matrix from `framework/languages/*/templates/`, filtered by language and template lists and by a pull request's changed paths |
| `use-local-packages.py` | Points a template **copy** at locally built Framework packages |
| `run-template.sh` | Makes one copy, runs the build and the e2e, and writes a result JSON |
| `summary.py` | Renders the results as a language, template, build, e2e and scenario-count table |

Templates use only public packages ([template authoring rules](../../framework/templates/TEMPLATE-AUTHORING.ko.md)),
so `source=local` edits only the running copy, never the template source. If a file in the copy
does not have the expected shape, `use-local-packages.py` fails immediately.

## Runs

- Pull request: only the changed templates, with public packages. A change to the workflow or to
  this directory runs every template.
- Manual run: `source` (public|local), `framework_version` (required for local), `languages`,
  `templates` (comma lists, empty means all). local builds the Framework packages of the checked-out
  commit at `framework_version` with the release workflow commands (not committed).
- C++ jobs use the runner in `vars.ZLINK_CPP_LINUX_RUNNER` only for manual runs; pull requests always
  run on GitHub-hosted runners.

Linking one copy locally:

```bash
cp -R framework/languages/node/templates/starter/my-game /tmp/node-starter
python3 scripts/templates/use-local-packages.py node /tmp/node-starter <packages-dir> 0.32.0 1.22.0
```
