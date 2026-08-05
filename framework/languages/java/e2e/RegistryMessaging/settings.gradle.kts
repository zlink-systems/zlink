pluginManagement {
    repositories {
        gradlePluginPortal()
        mavenCentral()
    }
}

apply(from = generateSequence(settingsDir) { it.parentFile }
    .first { it.resolve("gradle/zlink-local-packages.settings.gradle.kts").isFile }
    .resolve("gradle/zlink-local-packages.settings.gradle.kts"))

rootProject.name = "zlink-java-e2e-registry-messaging"

if (gradle.parent == null) {
    includeBuild("../..") {
        name = "zlink-framework-java-build"
    }
}

include(":Shared")
include(":Client")
include(":Server:Consumer")
include(":Server:ObjectClient")
include(":Server:Provider")
include(":Server:Workflow")
