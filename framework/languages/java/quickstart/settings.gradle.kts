pluginManagement {
    repositories {
        gradlePluginPortal()
        mavenCentral()
    }
}

dependencyResolutionManagement {
    repositories {
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
