pluginManagement {
    plugins {
        id("org.jetbrains.kotlin.jvm") version "2.1.0"
        id("org.jetbrains.kotlin.plugin.spring") version "2.1.0"
        id("com.google.protobuf") version "0.9.4"
    }
    repositories {
        gradlePluginPortal()
        mavenCentral()
    }
}

apply(from = generateSequence(settingsDir) { it.parentFile }
    .first { it.resolve("gradle/zlink-local-packages.settings.gradle.kts").isFile }
    .resolve("gradle/zlink-local-packages.settings.gradle.kts"))

rootProject.name = "zlink-kotlin-sample-bingo"

if (gradle.parent == null) {
    includeBuild("../../..") {
        name = "zlink-framework-java-build"
    }
}
include("Client")
include("Server:Api")
include("Server:Configuration")
include("Server:Matchmaking")
include("Server:Play")
include("Server:Session")
include("Shared")
