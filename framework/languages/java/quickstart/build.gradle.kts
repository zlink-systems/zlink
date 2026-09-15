plugins {
    // Declared once so every Kotlin subproject uses the same plugin version.
    alias(libs.plugins.kotlin.jvm) apply false
    alias(libs.plugins.kotlin.spring) apply false
}
