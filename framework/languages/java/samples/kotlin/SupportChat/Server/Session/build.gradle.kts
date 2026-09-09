plugins {
    application
    id("org.jetbrains.kotlin.jvm")
    kotlin("plugin.spring")
}

dependencies {
    implementation(project("${path.substringBefore(":Server")}:Shared"))
    implementation(project("${path.substringBefore(":Server")}:Server:Configuration"))
    implementation(zlinkLibs.zlink.framework.core)
    implementation(zlinkLibs.zlink.framework.kotlin)
    implementation(zlinkLibs.zlink.framework.locations.redis)
    implementation(zlinkLibs.zlink.framework.spring.boot.starter)
    implementation("org.springframework.boot:spring-boot-starter:3.5.14")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-core:1.9.0")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-jdk8:1.9.0")
    implementation(kotlin("stdlib"))
}


application {
    mainClass.set("systems.zlink.samples.kotlin.supportchat.server.session.ProgramKt")
    applicationDefaultJvmArgs = listOf("--enable-native-access=ALL-UNNAMED")
}
