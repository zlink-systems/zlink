plugins {
    application
    id("org.jetbrains.kotlin.jvm")
}

dependencies {
    implementation(project("${path.substringBeforeLast(":Client")}:Shared"))
    implementation(project("${path.substringBeforeLast(":Client")}:Server:Configuration"))
    implementation("systems.zlink:zlink-stream-connector:0.1.0-SNAPSHOT")
    implementation("systems.zlink:zlink-framework-kotlin:0.1.0-SNAPSHOT")
    implementation("systems.zlink:zlink-http-client-kotlin:0.3.1")
    implementation(zlinkLibs.zlink.bindings)
    implementation("io.netty:netty-buffer:4.1.100.Final")
}

kotlin {
    jvmToolchain(22)
}

application {
    mainClass.set("systems.zlink.samples.kotlin.deliverydispatch.client.ProgramKt")
    applicationDefaultJvmArgs = listOf("--enable-native-access=ALL-UNNAMED")
}
