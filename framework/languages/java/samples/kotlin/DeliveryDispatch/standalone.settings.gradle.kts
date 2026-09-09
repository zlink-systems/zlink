pluginManagement {
    plugins {
        id("org.jetbrains.kotlin.jvm") version "2.1.0"
    }
    repositories {
        gradlePluginPortal()
        mavenCentral()
    }
}

apply(from = settingsDir.resolve("../../gradle/zlink-sample-dependencies.settings.gradle.kts"))

rootProject.name = "zlink-kotlin-sample-deliverydispatch"

include("Client")
include("Server:Configuration")
include("Server:Registry")
include("Server:Dispatch")
include("Server:CourierSession")
include("Server:CourierSpotNode")
include("Server:Tracking")
include("Server:CustomerGateway")
include("Shared")
