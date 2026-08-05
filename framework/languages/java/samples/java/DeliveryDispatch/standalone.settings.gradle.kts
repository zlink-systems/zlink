pluginManagement {
    plugins {
        id("org.jetbrains.kotlin.jvm") version "2.1.0"
        id("org.jetbrains.kotlin.plugin.spring") version "2.1.0"
    }
    repositories {
        gradlePluginPortal()
        mavenCentral()
    }
}

apply(from = generateSequence(settingsDir) { it.parentFile }
    .first { it.resolve("gradle/zlink-local-packages.settings.gradle.kts").isFile }
    .resolve("gradle/zlink-local-packages.settings.gradle.kts"))

rootProject.name = "zlink-java-sample-deliverydispatch"

if (gradle.parent == null) {
    includeBuild("../../..") {
        name = "zlink-framework-java-build"
    }
}

include("Client")
include("Shared")
include("Server:Configuration")
include("Server:Tracking")
include("Server:CustomerGateway")
include("Server:CourierSession")
include("Server:CourierSpotNode")
include("Server:Dispatch")
