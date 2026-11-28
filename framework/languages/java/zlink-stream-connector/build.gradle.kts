plugins {
    `java-library`
    `maven-publish`
}

description = "ZLink Java STREAM connector core"

java {
    modularity.inferModulePath.set(true)
}

dependencies {
    api(project(":zlink-framework-json-internal"))
    api(zlinkLibs.zlink.bindings)
    api("com.fasterxml.jackson.core:jackson-databind:2.17.2")
    implementation("com.fasterxml.jackson.datatype:jackson-datatype-jsr310:2.17.2")
    api("io.netty:netty-buffer:4.1.100.Final")
    api("io.netty:netty-codec-http:4.1.100.Final")
    api("io.netty:netty-handler:4.1.100.Final")
    api("io.netty:netty-transport:4.1.100.Final")
    implementation("org.lz4:lz4-java:1.8.0")
    testImplementation(project(":zlink-framework-core"))
}

val buildNodeStreamConnector by tasks.registering(Exec::class) {
    workingDir = rootProject.file("../node")
    // Windows의 npm은 npm.cmd이며, Exec는 PATHEXT를 적용하지 않는다.
    val npm = if (System.getProperty("os.name").startsWith("Windows")) "npm.cmd" else "npm"
    commandLine(npm, "run", "build")
}

tasks.named("test") {
    dependsOn(buildNodeStreamConnector)
}
