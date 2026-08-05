plugins {
    application
    id("org.jetbrains.kotlin.jvm")
}

fun sampleProject(name: String) = project("${path.substringBeforeLast(":", "")}:$name")

dependencies {
    implementation(sampleProject("Shared"))
    implementation(sampleProject("Server:Configuration"))
    implementation("systems.zlink:zlink-framework-kotlin:0.1.0-SNAPSHOT")
    implementation("systems.zlink:zlink-http-client-kotlin:0.3.1")
    implementation("systems.zlink:zlink-stream-connector:0.1.0-SNAPSHOT")
    implementation(zlinkLibs.zlink.bindings)
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-core:1.9.0")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-jdk8:1.9.0")
}

kotlin {
    jvmToolchain(22)
}

application {
    mainClass.set("systems.zlink.samples.kotlin.gamequest.client.ProgramKt")
    applicationDefaultJvmArgs = listOf("--enable-native-access=ALL-UNNAMED")
}
