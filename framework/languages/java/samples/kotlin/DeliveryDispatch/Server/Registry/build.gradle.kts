plugins {
    application
    id("org.jetbrains.kotlin.jvm")
}

dependencies {
    implementation(project("${path.substringBefore(":Server")}:Server:Configuration"))
    implementation("systems.zlink:zlink-framework-core:0.10.0")
    implementation("systems.zlink:zlink-framework-spring-boot-starter:0.10.0")
    implementation("systems.zlink:zlink-framework-kotlin:0.10.0")
    implementation(zlinkLibs.zlink.bindings)
    implementation("org.springframework.boot:spring-boot-starter:3.5.14")
    implementation("io.netty:netty-buffer:4.1.100.Final")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-core:1.9.0")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-jdk8:1.9.0")
}

kotlin {
    jvmToolchain(22)
}

application {
    mainClass.set("systems.zlink.samples.kotlin.deliverydispatch.server.registry.ProgramKt")
    applicationDefaultJvmArgs = listOf("--enable-native-access=ALL-UNNAMED")
}
