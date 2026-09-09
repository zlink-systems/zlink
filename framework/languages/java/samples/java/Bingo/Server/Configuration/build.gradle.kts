plugins {
    `java-library`
}

dependencies {
    api(zlinkLibs.zlink.framework.locations.redis)
    api("io.micrometer:micrometer-core:1.15.8")
    api("org.springframework.boot:spring-boot:3.5.14")
    implementation("org.slf4j:slf4j-api:2.0.16")
}

java {
    toolchain {
        languageVersion.set(JavaLanguageVersion.of(22))
    }
}
