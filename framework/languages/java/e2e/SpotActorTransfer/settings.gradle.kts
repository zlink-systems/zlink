pluginManagement {
    repositories {
        gradlePluginPortal()
        mavenCentral()
    }
}

apply(from = generateSequence(settingsDir) { it.parentFile }
    .first { it.resolve("gradle/zlink-local-packages.settings.gradle.kts").isFile }
    .resolve("gradle/zlink-local-packages.settings.gradle.kts"))

rootProject.name = "zlink-java-e2e-spot-actor-transfer"

val packageMode = providers.gradleProperty("zlink.e2e.packageMode")
    .map(String::toBoolean)
    .orElse(
        providers.environmentVariable("ZLINK_E2E_PACKAGE_MODE")
            .map(String::toBoolean))
    .orElse(false)
    .get()

if (packageMode
    && !providers.environmentVariable("ZLINK_JAVA_BINDINGS_SOURCE")
        .orNull.isNullOrBlank()) {
    error("Package mode cannot use ZLINK_JAVA_BINDINGS_SOURCE.")
}

if (gradle.parent == null && !packageMode) {
    includeBuild("../..") {
        name = "zlink-framework-java-build"
    }
}

include(":Shared", ":Client", ":Server:ActorNode")
