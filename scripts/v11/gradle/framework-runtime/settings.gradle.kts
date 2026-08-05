pluginManagement {
    repositories {
        gradlePluginPortal()
        mavenCentral()
    }
}

val repositoryRoot = settingsDir.resolve("../../../..").canonicalFile
val javaRoot = repositoryRoot.resolve("framework/languages/java")

rootProject.name = "zlink-framework-java-runtime-regression"

// The framework build is deliberately nested. Its settings only include the
// sample composite when it is the top-level Gradle build.
includeBuild(javaRoot) {
    name = "zlink-framework-java-runtime"
}
