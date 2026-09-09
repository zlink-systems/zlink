pluginManagement {
    repositories {
        gradlePluginPortal()
        mavenCentral()
    }
}

apply(from = settingsDir.resolve("../../gradle/zlink-sample-dependencies.settings.gradle.kts"))

rootProject.name = "zlink-framework-java-shoppingmall-sample"

include(
    ":Client",
    ":Server:Configuration",
    ":Server:Shared",
    ":Server:CommerceApi",
    ":Server:OrderWorkflow",
    ":Shared",
)
