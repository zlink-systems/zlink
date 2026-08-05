pluginManagement {
    repositories {
        gradlePluginPortal()
        mavenCentral()
    }
}

rootProject.name = "zlink-kotlin-e2e-observability-ops"
apply(from = generateSequence(settingsDir) { it.parentFile }
    .first { it.resolve("gradle/zlink-local-packages.settings.gradle.kts").isFile }
    .resolve("gradle/zlink-local-packages.settings.gradle.kts"))

if (gradle.parent == null) {
    includeBuild("../..") { name = "zlink-framework-java-build" }
}

include(":Trigger", ":Verifier")
