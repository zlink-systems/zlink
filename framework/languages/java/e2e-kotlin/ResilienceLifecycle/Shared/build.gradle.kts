plugins {
    `java-library`
    id("org.jetbrains.kotlin.jvm")
}

dependencies {
    api("systems.zlink:zlink-framework-core:0.10.0")
    api("com.fasterxml.jackson.core:jackson-databind:2.17.2")
}

kotlin {
    jvmToolchain(22)
}
