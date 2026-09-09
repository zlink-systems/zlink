val frameworkRoot = generateSequence(settingsDir.parentFile) { it.parentFile }
    .firstOrNull { candidate ->
        candidate.resolve("settings.gradle.kts").isFile &&
            candidate.resolve("gradle/zlink-local-packages.settings.gradle.kts").isFile
    }

val packageMode = providers.gradleProperty("zlink.samples.packageMode")
    .map(String::toBoolean)
    .orElse(frameworkRoot == null)
    .get()
gradle.extensions.extraProperties["zlink.samples.effectivePackageMode"] = packageMode

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

if (!packageMode) {
    includeBuild(checkNotNull(frameworkRoot)) {
        name = "zlink-framework-java-build"
    }
}
