// Java Framework perf (perf/README.ko.md §17.2). One Gradle build; each role process is its own application project.
// The perf runner supplies the required Framework version and selects the existing sample package/source mode.
// Kotlin (§17.3) joins this build under kotlin/.
pluginManagement {
    repositories {
        gradlePluginPortal()
        mavenCentral()
    }
}

rootProject.name = "zlink-framework-perf-java"

val perfFrameworkVersion = providers.environmentVariable("ZLINK_PERF_FRAMEWORK_VERSION").orNull
    ?.takeIf { it.matches(Regex("[0-9]+\\.[0-9]+\\.[0-9]+")) }
    ?: error("ZLINK_PERF_FRAMEWORK_VERSION must be set to X.Y.Z for a perf build.")
val perfPackageSource = providers.environmentVariable("ZLINK_PERF_PACKAGE_SOURCE").orNull
    ?: error("ZLINK_PERF_PACKAGE_SOURCE must be set to published or local for a perf build.")
require(perfPackageSource == "published" || perfPackageSource == "local") {
    "ZLINK_PERF_PACKAGE_SOURCE must be published or local."
}

// scripts/build_role.sh maps these required values to the Gradle project properties of the shared sample settings.
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
