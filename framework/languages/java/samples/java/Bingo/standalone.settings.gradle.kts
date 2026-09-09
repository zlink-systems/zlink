pluginManagement {
    plugins {
        id("com.google.protobuf") version "0.9.4"
    }
    repositories {
        gradlePluginPortal()
        mavenCentral()
    }
}

apply(from = settingsDir.resolve("../../gradle/zlink-sample-dependencies.settings.gradle.kts"))

rootProject.name = "zlink-java-sample-bingo"

include("Client")
include("Server:Api")
include("Server:Configuration")
include("Server:Matchmaking")
include("Server:Play")
include("Server:Session")
include("Shared")
