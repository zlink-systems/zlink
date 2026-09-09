pluginManagement {
    repositories {
        gradlePluginPortal()
        mavenCentral()
    }
}

apply(from = settingsDir.resolve("../../gradle/zlink-sample-dependencies.settings.gradle.kts"))

rootProject.name = "zlink-framework-java-supportchat-sample"

include(
    ":Client",
    ":Server:Api",
    ":Server:Configuration",
    ":Server:Session",
    ":Server:Support",
    ":Shared",
)
