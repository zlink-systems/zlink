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
    implementation(zlinkLibs.zlink.framework.core)
    implementation(zlinkLibs.zlink.stream.connector)
    implementation(zlinkLibs.zlink.framework.codec.protobuf)
    implementation(zlinkLibs.zlink.bindings)
    implementation("com.fasterxml.jackson.core:jackson-databind:2.17.2")
    implementation("io.netty:netty-buffer:4.1.100.Final")
}


application {
    mainClass.set("systems.zlink.samples.bingo.client.Program")
    applicationDefaultJvmArgs = listOf("--enable-native-access=ALL-UNNAMED")
}
