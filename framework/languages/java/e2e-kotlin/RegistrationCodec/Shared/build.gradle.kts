plugins {
    id("org.jetbrains.kotlin.jvm")
}

kotlin {
    jvmToolchain(22)
}

dependencies {
    api("systems.zlink:zlink-framework-core:0.9.0")
    api("com.fasterxml.jackson.core:jackson-databind:2.17.2")
    api("com.google.protobuf:protobuf-java:4.30.2")
}
