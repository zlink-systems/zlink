pluginManagement { repositories { gradlePluginPortal(); mavenCentral() } }
apply(from = generateSequence(settingsDir) { it.parentFile }
    .first { it.resolve("gradle/zlink-local-packages.settings.gradle.kts").isFile }
    .resolve("gradle/zlink-local-packages.settings.gradle.kts"))
rootProject.name = "zlink-framework-perf"
includeBuild("..") { name = "zlink-framework-java-build" }
