plugins {
    id("org.jetbrains.kotlin.jvm")
}

dependencies {
    api("com.fasterxml.jackson.core:jackson-annotations:2.17.2")
    api("systems.zlink:zlink-framework-core:0.1.0-SNAPSHOT")
}

kotlin {
    jvmToolchain(22)
}
