plugins {
    `java-library`
}

dependencies {
    api("systems.zlink:zlink-framework-core:0.10.0")
}

java {
    toolchain {
        languageVersion.set(JavaLanguageVersion.of(22))
    }
}
