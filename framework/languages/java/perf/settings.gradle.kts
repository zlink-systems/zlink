// Java Framework perf (perf/README.ko.md §17.2). One Gradle build; each role process is its own application project.
// Package resolution is the samples' user mode (gradle.properties sets zlink.samples.packageMode=true): the published
// Framework and bindings packages, never a local Core or Framework build. Kotlin (§17.3) joins this build under kotlin/.
pluginManagement {
    repositories {
        gradlePluginPortal()
        mavenCentral()
    }
}

rootProject.name = "zlink-framework-perf-java"

apply(from = settingsDir.resolve("../samples/gradle/zlink-sample-dependencies.settings.gradle.kts"))

include(
    "shared",
    "server-support",
    "client",
    "session-actor-local-server",
    "session-server",
    "actor-server",
    "channel-server",
    "spot-server",
    "actor-caller-server",
    "publisher-server",
    "subscriber-server",
    "kotlin:shared",
    "kotlin:client",
    "kotlin:session-actor-local-server",
    "kotlin:session-server",
    "kotlin:actor-server",
    "kotlin:channel-server",
    "kotlin:spot-server",
    "kotlin:actor-caller-server",
    "kotlin:publisher-server",
    "kotlin:subscriber-server",
)
