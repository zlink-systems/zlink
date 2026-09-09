pluginManagement {
    repositories {
        gradlePluginPortal()
        mavenCentral()
    }
}

apply(from = settingsDir.resolve("../../gradle/zlink-sample-dependencies.settings.gradle.kts"))

rootProject.name = "zlink-framework-java-gamequest-sample"

include(
    ":Client",
    ":Server:Configuration",
    ":Server:GameApi",
    ":Server:QuestMission",
    ":Shared",
)
