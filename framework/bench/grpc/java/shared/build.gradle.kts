import com.google.protobuf.gradle.id

plugins {
    `java-library`
    id("com.google.protobuf") version "0.9.4"
}

dependencies {
    // spec section 3: gRPC and the ZLink framework carry the same protobuf DTO, and the
    // raw row puts the same encoded BenchPayload on the wire (FB-024).
    api("com.google.protobuf:protobuf-java:4.30.2")
    api("io.grpc:grpc-protobuf:1.72.0")
    api("io.grpc:grpc-stub:1.72.0")
    compileOnly("org.apache.tomcat:annotations-api:6.0.53")
    api(zlinkLibs.zlink.bindings)
}

protobuf {
    protoc { artifact = "com.google.protobuf:protoc:4.30.2" }
    plugins { id("grpc") { artifact = "io.grpc:protoc-gen-grpc-java:1.72.0" } }
    generateProtoTasks {
        all().forEach { it.plugins { id("grpc") } }
    }
}
