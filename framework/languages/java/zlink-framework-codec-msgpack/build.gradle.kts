plugins {
    `java-library`
    `maven-publish`
}

description = "ZLink Java framework MessagePack codec extension"

dependencies {
    api(project(":zlink-framework-core"))
    api(project(":zlink-stream-connector"))
    implementation("com.fasterxml.jackson.core:jackson-databind:2.17.2")
    testImplementation(project(":zlink-framework-codec-protobuf"))
    testImplementation("com.google.protobuf:protobuf-java:4.30.2")
}
