pluginManagement {
    repositories {
        gradlePluginPortal()
        mavenCentral()
    }
}

fun zlinkLocalMavenRepository(): java.io.File? {
    val configuredRoot =
        providers.gradleProperty("zlink.localPackageRoot")
            .orElse(providers.environmentVariable("ZLINK_LOCAL_PACKAGE_ROOT"))
            .orNull
    return configuredRoot?.takeIf { it.isNotBlank() }?.let { file(it).resolve("maven") }
}

dependencyResolutionManagement {
    repositories {
        zlinkLocalMavenRepository()?.let { localRepository ->
            maven { url = uri(localRepository) }
        }
        mavenCentral()
    }
}

rootProject.name = "zlink-quickstart"

// Java quickstart.
include("java:Shared")
include("java:Server")
include("java:Client")

// Kotlin quickstart. Kotlin has no directory of its own in this repository —
// it lives under framework/languages/java, next to the Java sources.
include("kotlin:Shared")
include("kotlin:Server")
include("kotlin:Client")
