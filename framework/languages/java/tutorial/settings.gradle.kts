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

rootProject.name = "zlink-tutorial"

// Java tutorial.
include("java:Shared")
include("java:Server")
include("java:Client")
include("java:StreamClient")

// Kotlin has no directory of its own in this repository -- it lives under
// framework/languages/java, next to the Java sources. The same layout the
// quickstart uses.
include("kotlin:Shared")
include("kotlin:Server")
include("kotlin:Client")
include("kotlin:StreamClient")
