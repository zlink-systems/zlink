plugins {
    `java-library`
}

dependencies {
    api("systems.zlink:zlink-framework-core:0.10.0")
    implementation("com.fasterxml.jackson.core:jackson-databind:2.17.2")
}

java {
    toolchain {
        languageVersion.set(JavaLanguageVersion.of(22))
    }
}
