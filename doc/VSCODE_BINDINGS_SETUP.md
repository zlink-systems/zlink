[English](VSCODE_BINDINGS_SETUP.md) | [한국어](VSCODE_BINDINGS_SETUP.ko.md)

# VS Code Bindings Setup

This workspace is configured so language-level code browsing works for:

- C/C++ (`bindings/cpp`, `bindings/node/native`)
- C# (`bindings/dotnet`)
- Java (`bindings/java`)
- JavaScript (`bindings/node`)
- Python (`bindings/python`)

## Required toolchains

- CMake + Ninja + C/C++ compiler
- .NET SDK (latest stable)
- Node.js (latest LTS, project requires `>=20`)
- Python (latest stable, project requires `>=3.9`)
- Adoptium JDK 22

## Java 22 setup (required)

Set `JDK22_HOME` to your Adoptium JDK 22 install path.
Set `JAVA_HOME` to the same path.

Windows PowerShell example:

```powershell
setx JDK22_HOME "C:\Program Files\Eclipse Adoptium\jdk-22.*"
setx JAVA_HOME "C:\Program Files\Eclipse Adoptium\jdk-22.*"
```

Restart VS Code after setting `JDK22_HOME`.

`bindings/java` includes Gradle Wrapper (`gradlew`, `gradlew.bat`), so a separate Gradle installation is optional.

On Windows, `zlink.dll` depends on OpenSSL runtime DLLs (`libcrypto-3-x64.dll`, `libssl-3-x64.dll`).
If they are not on your `PATH`, set one of these variables to the directory containing those DLLs:

- `ZLINK_OPENSSL_BIN` (used by Node binding loader)
- `PATH` (used by Java/Node fallback loader)

## VS Code

Open the repo root and install workspace recommendations from:

- `.vscode/extensions.json`

Run build/test tasks from:

- `Terminal` -> `Run Task`
- `.vscode/tasks.json`

Recommended test flow:

- `bindings: all test` for stable suites
- `bindings: java integration test` for Java stable integration scenarios
- `bindings: java integration discovery` for Java discovery/gateway/spot scenario
- `bindings: node integration discovery` for Node discovery/gateway/spot scenario
- `bindings: python integration discovery` for Python discovery/gateway/spot scenario
