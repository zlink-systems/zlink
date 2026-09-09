plugins {
    application
}

fun sampleProject(name: String) = project("${sampleRootPath()}:$name")

fun sampleRootPath(): String {
    val serverIndex = path.indexOf(":Server")
    return if (serverIndex >= 0) path.substring(0, serverIndex)
    else path.substringBeforeLast(":", "")
}

dependencies {
    implementation(sampleProject("Shared"))
    implementation(sampleProject("Server:Configuration"))
    implementation(zlinkLibs.zlink.framework.core)
    implementation(zlinkLibs.zlink.framework.codec.protobuf)
    implementation(zlinkLibs.zlink.framework.locations.redis)
    implementation(zlinkLibs.zlink.framework.spring.boot.starter)
    implementation("io.lettuce:lettuce-core:6.3.2.RELEASE")
    implementation("org.springframework.boot:spring-boot-starter:3.5.14")
}


application {
    mainClass.set("systems.zlink.samples.bingo.server.matchmaking.Program")
    applicationDefaultJvmArgs = listOf("--enable-native-access=ALL-UNNAMED")
}
