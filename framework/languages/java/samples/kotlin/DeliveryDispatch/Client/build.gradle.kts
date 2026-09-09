plugins {
    application
    id("org.jetbrains.kotlin.jvm")
}

dependencies {
    implementation(project("${path.substringBeforeLast(":Client")}:Shared"))
    implementation(project("${path.substringBeforeLast(":Client")}:Server:Configuration"))
    implementation(zlinkLibs.zlink.stream.connector)
    implementation(zlinkLibs.zlink.framework.kotlin)
    implementation(zlinkLibs.zlink.http.client.kotlin)
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
