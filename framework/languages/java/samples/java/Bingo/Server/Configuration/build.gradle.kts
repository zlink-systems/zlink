plugins {
    `java-library`
}

dependencies {
    api("systems.zlink:zlink-framework-locations-redis:0.1.0-SNAPSHOT")
    api("io.micrometer:micrometer-core:1.15.8")
    api("org.springframework.boot:spring-boot:3.5.14")
}

java {
    toolchain {
        languageVersion.set(JavaLanguageVersion.of(22))
    }
}
