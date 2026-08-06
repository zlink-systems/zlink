plugins {
    `java-library`
    id("com.google.protobuf")
}

dependencies {
    api("systems.zlink:zlink-framework-core:0.10.0")
    api("com.google.protobuf:protobuf-java:4.30.2")
}

protobuf {
    protoc {
        artifact = "com.google.protobuf:protoc:4.30.2"
    }
}

java {
    toolchain {
        languageVersion.set(JavaLanguageVersion.of(22))
    }
}
