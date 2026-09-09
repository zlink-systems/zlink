pluginManagement {
    plugins {
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
