plugins {
    `java-library`
}

dependencies {
    api("systems.zlink:zlink-framework-core:0.1.0-SNAPSHOT")
}

java {
    toolchain {
        languageVersion.set(JavaLanguageVersion.of(22))
    }
}
