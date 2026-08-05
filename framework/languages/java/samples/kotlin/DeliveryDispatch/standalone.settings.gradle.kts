pluginManagement {
    plugins {
        id("org.jetbrains.kotlin.jvm") version "2.1.0"
    }
    repositories {
        gradlePluginPortal()
        mavenCentral()
    }
}

apply(from = generateSequence(settingsDir) { it.parentFile }
    .first { it.resolve("gradle/zlink-local-packages.settings.gradle.kts").isFile }
    .resolve("gradle/zlink-local-packages.settings.gradle.kts"))

rootProject.name = "zlink-kotlin-sample-deliverydispatch"

if (gradle.parent == null) {
    includeBuild("../../..") {
        name = "zlink-framework-java-build"
    }
}

include("Client")
include("Server:Configuration")
include("Server:Registry")
include("Server:Dispatch")
include("Server:CourierSession")
include("Server:CourierSpotNode")
include("Server:Tracking")
include("Server:CustomerGateway")
include("Shared")
