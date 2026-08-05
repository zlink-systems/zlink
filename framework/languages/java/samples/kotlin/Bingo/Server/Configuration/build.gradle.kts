plugins {
    id("org.jetbrains.kotlin.jvm")
}

dependencies {
    api("org.springframework.boot:spring-boot:3.5.14")
    api("systems.zlink:zlink-framework-locations-redis:0.9.0")
    api("io.micrometer:micrometer-core:1.15.8")
}

kotlin {
    jvmToolchain(22)
}
