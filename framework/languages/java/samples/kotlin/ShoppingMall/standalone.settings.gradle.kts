pluginManagement {
    plugins {
    }
    repositories {
        gradlePluginPortal()
        mavenCentral()
    }
}

apply(from = settingsDir.resolve("../../gradle/zlink-sample-dependencies.settings.gradle.kts"))

rootProject.name = "zlink-kotlin-sample-shoppingmall"

include("Client")
include("Server:Configuration")
include("Server:CommerceApi")
include("Server:OrderWorkflow")
include("Shared")
