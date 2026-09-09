pluginManagement {
    plugins {
    }
    repositories {
        gradlePluginPortal()
        mavenCentral()
    }
}

apply(from = settingsDir.resolve("../../gradle/zlink-sample-dependencies.settings.gradle.kts"))

rootProject.name = "zlink-java-sample-tictactoe"

include("Client")
include("Server")
include("Shared")
