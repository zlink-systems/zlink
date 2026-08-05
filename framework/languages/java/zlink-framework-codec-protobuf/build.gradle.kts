plugins {
    `java-library`
    `maven-publish`
}

description = "ZLink Java framework Protobuf codec extension"

dependencies {
    api(project(":zlink-framework-core"))
    api(project(":zlink-stream-connector"))
    implementation("com.google.protobuf:protobuf-java:4.30.2")
    implementation("com.fasterxml.jackson.core:jackson-databind:2.17.2")
}
