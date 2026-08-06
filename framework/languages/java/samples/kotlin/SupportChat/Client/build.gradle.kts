plugins {
    application
    id("org.jetbrains.kotlin.jvm")
}

dependencies {
    implementation(project("${path.substringBeforeLast(":Client")}:Shared"))
    implementation(project("${path.substringBeforeLast(":Client")}:Server:Configuration"))
    implementation("systems.zlink:zlink-stream-connector:0.10.0")
    implementation("systems.zlink:zlink-framework-kotlin:0.10.0")
    implementation(zlinkLibs.zlink.bindings)
    implementation("com.fasterxml.jackson.core:jackson-databind:2.17.2")
    implementation("com.fasterxml.jackson.module:jackson-module-kotlin:2.17.2")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-core:1.9.0")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-jdk8:1.9.0")
}

kotlin {
    jvmToolchain(22)
}

application {
    mainClass.set("systems.zlink.samples.kotlin.supportchat.client.ProgramKt")
    applicationDefaultJvmArgs = listOf("--enable-native-access=ALL-UNNAMED")
}
