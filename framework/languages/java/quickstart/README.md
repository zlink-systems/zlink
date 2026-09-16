# ZLink Java/Kotlin quickstart

Same scenario as `framework/doc/framework/common/guide/server/02-getting-started.ko.md`
§2 "최소 예제 — 두 process가 서로 호출한다", and the same shape as the working
`framework/languages/dotnet/quickstart/` reference: no Redis, no location store, two
processes that address each other by a hand-written endpoint and exchange one
request/reply. This directory holds **both** a Java and a Kotlin copy of that scenario,
laid out the way `framework/languages/java/samples/` separates them — a `java/` subtree
and a `kotlin/` subtree under one Gradle build (Kotlin has no directory of its own in
this repository). Both build from Maven Central only; nothing here references this
repository's own source or local package cache.

## Prerequisites

- **JDK 25 or newer to run it, Gradle 9.3 (bundled wrapper) to build it.**
  The getting-started guide says "JDK 25 이상" for Java/Kotlin, and the actual class
  files published for `zlink-framework-core`, `zlink-framework-spring-boot-starter`,
  `zlink-framework-kotlin` and `zlink` 0.12.0/1.1.0 all carry class file major
  version **69**, which is Java 25 (up from major 66 / Java 22 at 0.11.0/0.17.6) —
  checked by unzipping each jar and reading every `.class` file's major version
  (`cafe babe 0000 0045`; `0x45` = 69 = 25), not just one sample class. A JVM older
  than 25 refuses to load them (`UnsupportedClassVersionError`). This quickstart pins
  the Gradle toolchain to 25 explicitly (`JavaLanguageVersion.of(25)` /
  `kotlin { jvmToolchain(25) }`) and was built and run with Temurin 25.0.4.1 on this
  machine — the same JDK 25 baseline
  `framework/languages/java/samples` already builds against
  (`gradle/zlink-jvm-baseline.settings.gradle.kts`), and now also a requirement of the
  published artifacts themselves, not just that build's own choice.
- Internet access to `repo1.maven.org` (Maven Central) the first time you build;
  `settings.gradle.kts` only declares `mavenCentral()`.
- No Redis, no other external dependency.

## Layout

- `java/Shared`, `java/Server`, `java/Client` — the Java copy.
- `kotlin/Shared`, `kotlin/Server`, `kotlin/Client` — the Kotlin copy, package-for-package
  the same scenario.
- `gradle/libs.versions.toml` — every package version pinned as an explicit number in
  one place, no conditionals. `systems.zlink:zlink` (the core binding) is deliberately
  **not** pinned here; it is left to resolve transitively from `zlink-framework-core`'s
  POM, which depends on exactly `zlink:1.1.0`.
- `Server` in each language listens on `tcp://0.0.0.0:7101` and handles the `greeting`
  channel. `Client` listens on `tcp://0.0.0.0:7102`, connects to the server manually,
  and exposes `GET /hello/{name}` on `http://127.0.0.1:5080`. Because the two copies
  reuse the guide's own mesh port numbers, run only one language's Server+Client pair
  at a time.

## Checked before building — what's actually published

Maven Central's `maven-metadata.xml` for each artifact (checked 2026-09-15, after
Central propagation for the 0.12.0 release finished — `<latest>`/`<release>` both read
0.12.0):

| Artifact | Published versions | Used here |
| --- | --- | --- |
| `systems.zlink:zlink-framework-core` | 0.10.0, 0.11.0, 0.12.0 | 0.12.0 |
| `systems.zlink:zlink-framework-spring-boot-starter` | 0.10.0, 0.11.0, 0.12.0 | 0.12.0 |
| `systems.zlink:zlink-framework-kotlin` | 0.10.0, 0.11.0, 0.12.0 | 0.12.0 |
| `systems.zlink:zlink` | 0.17.3, 0.17.5, 0.17.6, 0.18.0, 1.0.0, 1.1.0 | 1.1.0 (transitive, not pinned) |

`zlink-framework-core-0.12.0.pom` depends on exactly `zlink:1.1.0` (compile scope) —
up from `zlink:0.17.6` at 0.11.0. `zlink-framework-spring-boot-starter` and
`zlink-framework-kotlin` both depend on `zlink-framework-core:0.12.0`, so the whole
tree agrees on `zlink:1.1.0`, which is also the latest published `zlink`. There is
therefore nothing left to pin around: the earlier concern (a newer `zlink` pinned
directly leaves peer admission unresponsive, as the .NET quickstart found against
0.11.0/0.17.6) no longer applies once the transitive resolution itself lands on the
latest binding. Confirmed by actually building and running both the Java and Kotlin
copies against unpinned resolution — see "Build and run" below.

`framework/languages/java/samples/gradle/zlink-sample-dependencies.settings.gradle.kts`
defaults `zlink.frameworkVersion` to `"0.11.1"` in package mode — that version is still
**not** published (Maven Central has 0.10.0/0.11.0/0.12.0, no 0.11.1). Worth flagging to
whoever maintains that default; out of scope for this directory.

## Build and run

```bash
cd framework/languages/java/quickstart

# Java
./gradlew :java:Server:installDist :java:Client:installDist
./java/Server/build/install/Server/bin/Server &
./java/Client/build/install/Client/bin/Client &
curl http://127.0.0.1:5080/hello/world
# stop both processes, then:

# Kotlin
./gradlew :kotlin:Server:installDist :kotlin:Client:installDist
./kotlin/Server/build/install/Server/bin/Server &
./kotlin/Client/build/install/Client/bin/Client &
curl http://127.0.0.1:5080/hello/world
```

## Expected output

```
$ curl http://127.0.0.1:5080/hello/world
hello, world
```

HTTP 200, for both the Java and the Kotlin Client.

## What to take when porting into your own project

- `gradle/libs.versions.toml` — the version-pinning approach and the actual numbers
  (and the comment on why `zlink` itself is left unpinned).
- `Shared/…/Hello.*`, `Shared/…/Greeting.*` — the contract shape (Java records / Kotlin
  data classes).
- `Server/…/ServerApplication.*` — the `@EnableZLinkFramework @SpringBootApplication`
  + `@Bean ZLinkFrameworkConfigurer` block: mesh name, `listen(...)`,
  `channelName(...).server().addRequestHandler(...)`, and (non-web process only) the
  `SpringApplication#setKeepAlive(true)` call in `main`.
- `Client/…/ClientApplication.*` — the `channelName(...).client()` +
  `peerConnections().connect(...)` block, and the `IZLinkRouteClient`/`ZLinkRouteClient`
  call site.
- A real service normally replaces the manual `peerConnections().connect(...)` with a
  location store (Redis, etc.) — left out here on purpose, since this quickstart only
  confirms the install works.

## Where this differs from guide §2, and why (needed to actually get it running)

The guide's Java/Kotlin snippets are a minimized sketch; reproducing them literally
against the actually-published 0.12.0 artifacts hits six points where a real project
needs something more. None of these are guessed — every one reproduced a concrete error
first.

At 0.11.0 there was a seventh point here: `zlink` had to be left unpinned specifically
to avoid an admission-handshake timeout that pinning the latest published `zlink`
(1.1.0) directly would have caused. That's gone at 0.12.0 — `zlink-framework-core`'s
own POM now depends on `zlink:1.1.0`, the latest, so transitive resolution and "pin the
latest yourself" land on the same version. `zlink` is still left unpinned here (see
"Checked before building" above), but only as the ordinary practice of not repeating a
transitive version by hand, not to dodge a bug.

1. **The Server process needs to be told to stay alive (both languages).** Neither
   `Server` exposes HTTP (only `spring-boot-starter`, not `-web`), so nothing keeps the
   JVM alive after Spring's context finishes refreshing — `main` returns, there is no
   other non-daemon thread, and the process exits right after
   `ZLINK_FRAMEWORK_READY`, before ever handling a request. Confirmed by running it
   as-is: the process logged `ZLINK_FRAMEWORK_READY` then `ZLINK_FRAMEWORK_TERMINATION
   outcome=STOPPED` and exited within a second. Fix: `SpringApplication#setKeepAlive(true)`
   (Spring Boot 3.2+) before calling `run(...)` — the same thing
   `framework/languages/java/samples`' own `SampleApplication.start(...)` does
   (`builder.application().setKeepAlive(true)`). The guide's snippet shows no `main`
   method at all for the Java/Kotlin server, so this is invisible until you actually
   try to run it.

2. **`@PathVariable` needs `-parameters` (Java Client only).** Without
   `options.compilerArgs.add("-parameters")` on `compileJava`, every
   `GET /hello/{name}` call 500s with `IllegalArgumentException: Name for argument of
   type [java.lang.String] not specified, ... Ensure that the compiler uses the
   '-parameters' flag.` — javac drops parameter names by default and Spring MVC has
   nothing else to match `{name}` against. `java/Client/build.gradle.kts` sets this
   explicitly.

3. **Kotlin's `@SpringBootApplication`/`@RestController` classes must not be `final`
   (Kotlin Client and Server).** Spring proxies `@Configuration` classes with CGLIB,
   which needs a non-final class; Kotlin classes are `final` by default. Left as-is,
   startup fails immediately with `BeanDefinitionParsingException: Configuration
   problem: @Configuration class 'ServerApplication' may not be final.` Fix: the
   `org.jetbrains.kotlin.plugin.spring` Gradle plugin (applied in both
   `kotlin/Server/build.gradle.kts` and `kotlin/Client/build.gradle.kts`), which opens
   Spring-annotated classes automatically — the same plugin
   `framework/languages/java/samples/build.gradle.kts` declares at the root for exactly
   this reason.

4. **A Kotlin `suspend fun` controller needs `kotlinx-coroutines-reactor` on the
   classpath (Kotlin Client only).** Spring MVC bridges a suspending handler method
   through `InvokeSuspendingFunction`, which needs `org.reactivestreams.Publisher` even
   though the handler body never touches Reactor. Left out, every call 500s with
   `NoClassDefFoundError: org/reactivestreams/Publisher`. Fix: add
   `org.jetbrains.kotlinx:kotlinx-coroutines-reactor` (version left to the
   `kotlinx-coroutines-bom` constraint that `zlink-framework-kotlin` already pulls in,
   so it resolves to 1.9.0 without being pinned again). The guide's Kotlin client
   snippet uses this exact `suspend fun hello(...)` shape, so any faithful port hits
   this.

5. **The mesh's default JSON codec needs `jackson-module-kotlin` on the Server's
   classpath to decode a Kotlin data class (Kotlin Server only).** The default codec
   (`ZLinkJsonMessageSerializer` / `ZLinkFrameworkJsonProfile`, inside
   `zlink-framework-core`) builds its `ObjectMapper` with Jackson's
   `findAndAddModules()` — i.e. whatever Jackson modules happen to be on the classpath
   via `ServiceLoader`. The Kotlin `Client` gets `jackson-module-kotlin` for free
   (Spring Boot's web starter pulls it in transitively), but the Kotlin `Server` (no
   web starter) does not, so it cannot construct a `Hello` data class from the incoming
   JSON at all. Confirmed: every call failed with
   `ZLinkFrameworkException: PayloadDecodeFailed: failed to decode payload for
   'greeting:Hello'.` until `jackson-module-kotlin` was added to
   `kotlin/Server/build.gradle.kts` explicitly (pinned to 2.17.2 to match the
   `jackson-databind` version `zlink-framework-core:0.12.0` itself depends on —
   unchanged from 0.11.0).
   **Anything that registers a Kotlin data class as a channel contract needs this on
   every process that (de)serializes it, not just the one exposing HTTP.**

6. **The guide's Kotlin server handler doesn't type-check against the published API
   (Kotlin Server only).** The guide shows:
   ```kotlin
   class HelloHandler : ZLinkRequestHandler<Hello, Greeting> {
       override suspend fun handle(request: Hello, context: ZLinkMessageContext): Greeting = ...
   }
   ```
   but the published `systems.zlink.framework.channels.ZLinkRequestHandler<TRequest,
   TReply>` is the plain Java interface with
   `CompletionStage<TReply> handle(TRequest, ZLinkMessageContext)` — a Kotlin `suspend
   fun` cannot override that (different generated JVM signature: an extra
   `Continuation` parameter). The actual coroutine-native interface,
   `systems.zlink.framework.kotlin.ZLinkSuspendingRequestHandler<TRequest, TReply>`
   (published in `zlink-framework-kotlin`, source-verified against its sources jar),
   is never wired to `ZLinkMeshChannelServerBuilder.addRequestHandler(...)` — that
   method's generic bound requires exactly the Java `ZLinkRequestHandler`. This
   quickstart's `HelloHandler` therefore implements the Java interface directly and
   returns `CompletableFuture.completedFuture(...)`, same as the Java quickstart's
   handler, instead of the guide's `suspend fun` shape.
