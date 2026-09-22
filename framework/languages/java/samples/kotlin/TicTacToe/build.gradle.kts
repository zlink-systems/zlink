plugins {
    base
    id("dev.detekt") apply false
    id("org.jetbrains.kotlin.jvm") apply false
}

val kotlinDetektScript =
    generateSequence(rootProject.projectDir) { it.parentFile }
        .map { it.resolve("gradle/kotlin-detekt.gradle") }
        .first { it.isFile }

subprojects { apply(from = kotlinDetektScript) }
