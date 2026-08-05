pluginManagement {
    repositories {
        gradlePluginPortal()
        mavenCentral()
    }
}

apply(from = generateSequence(settingsDir) { it.parentFile }
    .first { it.resolve("gradle/zlink-local-packages.settings.gradle.kts").isFile }
    .resolve("gradle/zlink-local-packages.settings.gradle.kts"))

rootProject.name = "zlink-framework-java-shoppingmall-sample"

includeBuild("../../..") {
    name = "zlink-framework-java-build"
}

include(
    ":Client",
    ":Server:Configuration",
    ":Server:Shared",
    ":Server:CommerceApi",
    ":Server:OrderWorkflow",
    ":Shared",
)
