plugins {
    id("org.jetbrains.kotlin.jvm")
}

dependencies {
    api("com.fasterxml.jackson.core:jackson-annotations:2.17.2")
    api("systems.zlink:zlink-framework-core:0.9.0")
}

kotlin {
    jvmToolchain(22)
}
