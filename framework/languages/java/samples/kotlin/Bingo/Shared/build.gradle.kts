plugins {
    id("org.jetbrains.kotlin.jvm")
    id("com.google.protobuf")
}

dependencies {
    api("systems.zlink:zlink-framework-core:0.9.0")
    api("systems.zlink:zlink-framework-kotlin:0.9.0")
    api("com.google.protobuf:protobuf-java:4.30.2")
}

protobuf {
    protoc {
        artifact = "com.google.protobuf:protoc:4.30.2"
    }
}

kotlin {
    jvmToolchain(22)
}
