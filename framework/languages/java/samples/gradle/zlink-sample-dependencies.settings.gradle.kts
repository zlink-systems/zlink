val frameworkRoot = generateSequence(settingsDir.parentFile) { it.parentFile }
    .firstOrNull { candidate ->
        candidate.resolve("settings.gradle.kts").isFile &&
            candidate.resolve("gradle/zlink-local-packages.settings.gradle.kts").isFile
    }

val packageMode = providers.gradleProperty("zlink.samples.packageMode")
    .map(String::toBoolean)
    .orElse(frameworkRoot == null)
    .get()
val frameworkVersionDefault = if (packageMode) {
    providers.provider { "0.11.0" }
} else {
    val localFrameworkRoot = checkNotNull(frameworkRoot) {
        "Developer mode requires the zlink Java framework source above the samples directory. " +
            "Use -Pzlink.samples.packageMode=true for published packages."
    }
    val versionFile = localFrameworkRoot.resolve("../../../FRAMEWORK_VERSION").canonicalFile
    val versionFileRelativeToSettings = settingsDir.toPath()
        .relativize(versionFile.toPath())
        .toString()
    providers.fileContents(layout.settingsDirectory.file(versionFileRelativeToSettings))
        .asText
        .map { contents ->
            requireNotNull(
                Regex("""ZLINK_FRAMEWORK_VERSION=([0-9]+\.[0-9]+\.[0-9]+)""")
                    .matchEntire(contents.trim())
            ) { "FRAMEWORK_VERSION must contain exactly ZLINK_FRAMEWORK_VERSION=X.Y.Z" }
                .groupValues[1]
        }
}
val frameworkVersion = providers.gradleProperty("zlink.frameworkVersion")
    .orElse(frameworkVersionDefault)
    .get()
gradle.extensions.extraProperties["zlink.samples.effectivePackageMode"] = packageMode
gradle.extensions.extraProperties["zlink.samples.frameworkVersion"] = frameworkVersion

if (packageMode && !providers.environmentVariable("ZLINK_JAVA_BINDINGS_SOURCE").orNull.isNullOrBlank()) {
    error("Package mode cannot use ZLINK_JAVA_BINDINGS_SOURCE.")
}

if (packageMode) {
    val bindingsVersion = providers.gradleProperty("zlink.bindingsVersion")
        .orElse("0.17.6")
        .get()
    dependencyResolutionManagement {
        versionCatalogs {
            create("zlinkLibs") {
                library("zlink-bindings", "systems.zlink", "zlink").version(bindingsVersion)
            }
        }
        repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
        repositories {
            mavenCentral()
        }
    }
} else {
    val localFrameworkRoot = checkNotNull(frameworkRoot) {
        "Developer mode requires the zlink Java framework source above the samples directory. " +
            "Use -Pzlink.samples.packageMode=true for published packages."
    }
    apply(from = localFrameworkRoot.resolve("gradle/zlink-local-packages.settings.gradle.kts"))
}

dependencyResolutionManagement {
    versionCatalogs.named("zlinkLibs") {
        version("zlink-framework", frameworkVersion)
        library("zlink-framework-core", "systems.zlink", "zlink-framework-core")
            .versionRef("zlink-framework")
        library("zlink-framework-codec-protobuf", "systems.zlink", "zlink-framework-codec-protobuf")
            .versionRef("zlink-framework")
        library("zlink-framework-locations-redis", "systems.zlink", "zlink-framework-locations-redis")
            .versionRef("zlink-framework")
        library("zlink-framework-spring-boot-starter", "systems.zlink", "zlink-framework-spring-boot-starter")
            .versionRef("zlink-framework")
        library("zlink-framework-kotlin", "systems.zlink", "zlink-framework-kotlin")
            .versionRef("zlink-framework")
        library("zlink-http-client-java", "systems.zlink", "zlink-http-client")
            .versionRef("zlink-framework")
        library("zlink-http-client-kotlin", "systems.zlink", "zlink-http-client-kotlin")
            .versionRef("zlink-framework")
        library("zlink-stream-connector", "systems.zlink", "zlink-stream-connector")
            .versionRef("zlink-framework")
    }
}

if (!packageMode) {
    includeBuild(checkNotNull(frameworkRoot)) {
        name = "zlink-framework-java-build"
    }
}
