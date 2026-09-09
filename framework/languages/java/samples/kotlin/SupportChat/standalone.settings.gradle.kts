pluginManagement {
    plugins {
    }
    repositories {
        gradlePluginPortal()
        mavenCentral()
    }
}

apply(from = settingsDir.resolve("../../gradle/zlink-sample-dependencies.settings.gradle.kts"))

rootProject.name = "zlink-kotlin-sample-supportchat"

include("Shared")
include("Client")
include("Server:Configuration")
include("Server:Api")
include("Server:Session")
include("Server:Support")
