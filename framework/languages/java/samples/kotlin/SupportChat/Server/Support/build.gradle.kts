plugins {
    application
    id("org.jetbrains.kotlin.jvm")
    kotlin("plugin.spring")
}

dependencies {
    implementation(project("${path.substringBefore(":Server")}:Shared"))
    implementation(project("${path.substringBefore(":Server")}:Server:Configuration"))
    implementation("systems.zlink:zlink-framework-core:0.9.0")
    implementation("systems.zlink:zlink-framework-kotlin:0.9.0")
    implementation("systems.zlink:zlink-framework-locations-redis:0.9.0")
    implementation("systems.zlink:zlink-framework-spring-boot-starter:0.9.0")
    implementation("org.springframework.boot:spring-boot-starter:3.5.14")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-core:1.9.0")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-jdk8:1.9.0")
    implementation(kotlin("stdlib"))
}

kotlin {
    jvmToolchain(22)
}

application {
    mainClass.set("systems.zlink.samples.kotlin.supportchat.server.support.ProgramKt")
    applicationDefaultJvmArgs = listOf("--enable-native-access=ALL-UNNAMED")
}
