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
    implementation("systems.zlink:zlink-framework-spring-boot-starter:0.1.0-SNAPSHOT")
    implementation(zlinkLibs.zlink.bindings)
    implementation("com.fasterxml.jackson.core:jackson-databind:2.17.2")
    implementation("org.springframework.boot:spring-boot-starter:3.5.14")
    implementation("io.netty:netty-buffer:4.1.100.Final")
}

java {
    toolchain {
        languageVersion.set(JavaLanguageVersion.of(22))
    }
}

application {
    mainClass.set("systems.zlink.samples.deliverydispatch.server.tracking.Program")
    applicationDefaultJvmArgs = listOf("--enable-native-access=ALL-UNNAMED")
}
