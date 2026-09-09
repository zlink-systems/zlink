plugins {
    application
}

fun sampleProject(name: String) = project("${sampleRootPath()}:$name")

fun sampleRootPath(): String {
    val serverIndex = path.indexOf(":Server")
    return if (serverIndex >= 0) {
        path.substring(0, serverIndex)
    } else {
        path.substringBeforeLast(":", "")
    }
}

dependencies {
    implementation(sampleProject("Shared"))
    implementation(sampleProject("Server:Configuration"))
    implementation(zlinkLibs.zlink.framework.core)
    implementation(zlinkLibs.zlink.framework.codec.protobuf)
    implementation(zlinkLibs.zlink.framework.locations.redis)
    implementation(zlinkLibs.zlink.framework.spring.boot.starter)
    implementation(zlinkLibs.zlink.stream.connector)
    implementation(zlinkLibs.zlink.bindings)
    implementation("com.fasterxml.jackson.core:jackson-databind:2.17.2")
    implementation("io.lettuce:lettuce-core:6.3.2.RELEASE")
    implementation("org.springframework.boot:spring-boot-starter:3.5.14")
    implementation("org.springframework.boot:spring-boot-starter-actuator:3.5.14")
    implementation("io.netty:netty-buffer:4.1.100.Final")
    testImplementation(platform("org.junit:junit-bom:5.11.4"))
    testImplementation("org.junit.jupiter:junit-jupiter")
    testRuntimeOnly("org.junit.platform:junit-platform-launcher")
}


application {
    mainClass.set("systems.zlink.samples.bingo.server.play.Program")
    applicationDefaultJvmArgs = listOf("--enable-native-access=ALL-UNNAMED")
}

tasks.withType<Test>().configureEach {
    useJUnitPlatform()
}
