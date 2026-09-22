plugins {
    // Declared once so every Kotlin subproject uses the same plugin version.
    alias(libs.plugins.kotlin.jvm) apply false
    alias(libs.plugins.kotlin.spring) apply false
    id("dev.detekt") version "2.0.0-alpha.3" apply false
}

subprojects {
    plugins.withId("org.jetbrains.kotlin.jvm") {
        apply(from = rootProject.file("../gradle/kotlin-detekt.gradle"))
    }
}
