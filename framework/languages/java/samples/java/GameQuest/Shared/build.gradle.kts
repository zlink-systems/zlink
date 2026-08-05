plugins {
    `java-library`
}

dependencies {
    api("com.fasterxml.jackson.core:jackson-annotations:2.17.2")
}

java {
    toolchain {
        languageVersion.set(JavaLanguageVersion.of(22))
    }
}
