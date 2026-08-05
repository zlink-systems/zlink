plugins {
    `java-library`
    id("org.jetbrains.kotlin.jvm")
}

java {
    toolchain {
        languageVersion.set(JavaLanguageVersion.of(22))
    }
}

kotlin {
    jvmToolchain(22)
}
