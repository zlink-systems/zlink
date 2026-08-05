plugins {
    application
    id("org.jetbrains.kotlin.jvm")
    kotlin("plugin.spring")
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
    implementation("systems.zlink:zlink-framework-core:0.1.0-SNAPSHOT")
    implementation("systems.zlink:zlink-framework-codec-protobuf:0.1.0-SNAPSHOT")
    implementation("systems.zlink:zlink-framework-locations-redis:0.1.0-SNAPSHOT")
    implementation("systems.zlink:zlink-framework-spring-boot-starter:0.1.0-SNAPSHOT")
    implementation("systems.zlink:zlink-framework-kotlin:0.1.0-SNAPSHOT")
    implementation(zlinkLibs.zlink.bindings)
    implementation("com.fasterxml.jackson.core:jackson-databind:2.17.2")
    implementation("com.fasterxml.jackson.module:jackson-module-kotlin:2.17.2")
    implementation("io.lettuce:lettuce-core:6.3.2.RELEASE")
    implementation("org.springframework.boot:spring-boot-starter:3.5.14")
    implementation("org.springframework.boot:spring-boot-starter-actuator:3.5.14")
    implementation("io.netty:netty-buffer:4.1.100.Final")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-core:1.9.0")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-jdk8:1.9.0")
    implementation(kotlin("stdlib"))
}

kotlin {
    jvmToolchain(22)
}

application {
    mainClass.set("systems.zlink.samples.kotlin.bingo.server.play.ProgramKt")
    applicationDefaultJvmArgs = listOf("--enable-native-access=ALL-UNNAMED")
}
