pluginManagement {
    repositories {
        gradlePluginPortal()
        mavenCentral()
    }
}

apply(from = settingsDir.resolve("../../gradle/zlink-sample-dependencies.settings.gradle.kts"))

rootProject.name = "zlink-framework-java-zoneworld-sample"

include(
    ":Client",
    ":Server",
    ":Shared",
)
