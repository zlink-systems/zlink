pluginManagement {
    repositories {
        gradlePluginPortal()
        mavenCentral()
    }
}

apply(from = generateSequence(settingsDir) { it.parentFile }
    .first { it.resolve("gradle/zlink-local-packages.settings.gradle.kts").isFile }
    .resolve("gradle/zlink-local-packages.settings.gradle.kts"))

rootProject.name = "zlink-framework-java-supportchat-sample"

includeBuild("../../..") {
    name = "zlink-framework-java-build"
}

include(
    ":Client",
    ":Server:Api",
    ":Server:Configuration",
    ":Server:Session",
    ":Server:Support",
    ":Shared",
)
