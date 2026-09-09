pluginManagement {
    plugins {
    }
    repositories {
        gradlePluginPortal()
        mavenCentral()
    }
}

apply(from = generateSequence(settingsDir) { it.parentFile }
    .first { it.resolve("gradle/zlink-local-packages.settings.gradle.kts").isFile }
    .resolve("gradle/zlink-local-packages.settings.gradle.kts"))

rootProject.name = "zlink-kotlin-e2e-spot-actor-transfer"

if (gradle.parent == null) {
    includeBuild("../..") {
        name = "zlink-framework-java-build"
    }
}

include(":Shared", ":JavaClient", ":Client", ":Server:ActorNode")
project(":Shared").projectDir = file("../../e2e/SpotActorTransfer/Shared")
project(":JavaClient").projectDir = file("../../e2e/SpotActorTransfer/Client")
