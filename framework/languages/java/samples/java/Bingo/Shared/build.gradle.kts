plugins {
    `java-library`
    id("com.google.protobuf")
}

dependencies {
    api(zlinkLibs.zlink.framework.core)
    api("com.google.protobuf:protobuf-java:4.30.2")
}

protobuf {
    protoc {
        artifact = "com.google.protobuf:protoc:4.30.2"
    }
}
