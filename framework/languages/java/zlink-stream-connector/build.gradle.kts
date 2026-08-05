plugins {
    `java-library`
    `maven-publish`
}

description = "ZLink Java STREAM connector core"

java {
    modularity.inferModulePath.set(true)
}

dependencies {
    api(zlinkLibs.zlink.bindings)
    api("com.fasterxml.jackson.core:jackson-databind:2.17.2")
    implementation("com.fasterxml.jackson.datatype:jackson-datatype-jsr310:2.17.2")
    api("io.netty:netty-buffer:4.1.100.Final")
    api("io.netty:netty-codec-http:4.1.100.Final")
    api("io.netty:netty-handler:4.1.100.Final")
    api("io.netty:netty-transport:4.1.100.Final")
    implementation("org.lz4:lz4-java:1.8.0")
    implementation("io.micrometer:micrometer-core:1.15.8")
    testImplementation(project(":zlink-framework-core"))
}
