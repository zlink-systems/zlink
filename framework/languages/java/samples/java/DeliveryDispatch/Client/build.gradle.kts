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
    implementation("systems.zlink:zlink-framework-core:0.1.0-SNAPSHOT")
    implementation("systems.zlink:zlink-http-client:0.3.1")
    implementation("systems.zlink:zlink-stream-connector:0.1.0-SNAPSHOT")
    implementation(zlinkLibs.zlink.bindings)
    implementation("io.netty:netty-buffer:4.1.100.Final")
}

java {
    toolchain {
        languageVersion.set(JavaLanguageVersion.of(22))
    }
}

application {
    mainClass.set("systems.zlink.samples.deliverydispatch.client.Program")
    applicationDefaultJvmArgs = listOf("--enable-native-access=ALL-UNNAMED")
}
