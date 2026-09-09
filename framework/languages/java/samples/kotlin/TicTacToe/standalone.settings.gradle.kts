pluginManagement {
    plugins {
    }
    repositories {
        gradlePluginPortal()
        mavenCentral()
    }
}

apply(from = settingsDir.resolve("../../gradle/zlink-sample-dependencies.settings.gradle.kts"))

rootProject.name = "zlink-kotlin-sample-tictactoe"

include("Client")
include("Server")
include("Shared")
