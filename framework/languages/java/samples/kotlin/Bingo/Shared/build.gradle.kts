plugins {
    id("org.jetbrains.kotlin.jvm")
    id("com.google.protobuf")
}

dependencies {
    api(zlinkLibs.zlink.framework.core)
    api(zlinkLibs.zlink.framework.kotlin)
    api("com.google.protobuf:protobuf-java:4.30.2")
}

protobuf {
    protoc {
        artifact = "com.google.protobuf:protoc:4.30.2"
    }
}
