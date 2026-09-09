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

apply(from = settingsDir.resolve("../../gradle/zlink-sample-dependencies.settings.gradle.kts"))

rootProject.name = "zlink-java-sample-deliverydispatch"

include("Client")
include("Shared")
include("Server:Configuration")
include("Server:Tracking")
include("Server:CustomerGateway")
include("Server:CourierSession")
include("Server:CourierSpotNode")
include("Server:Dispatch")
