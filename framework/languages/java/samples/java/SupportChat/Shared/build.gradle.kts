plugins {
    `java-library`
}

dependencies {
    api(zlinkLibs.zlink.framework.core)
}

java {
    toolchain {
        languageVersion.set(JavaLanguageVersion.of(22))
    }
}
