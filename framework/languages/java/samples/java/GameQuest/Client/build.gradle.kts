plugins {
    application
}

fun sampleProject(name: String) = project("${path.substringBeforeLast(":", "")}:$name")

dependencies {
    implementation(sampleProject("Shared"))
    implementation(sampleProject("Server:Configuration"))
    implementation("systems.zlink:zlink-http-client:0.3.1")
    implementation("systems.zlink:zlink-stream-connector:0.1.0-SNAPSHOT")
    implementation(zlinkLibs.zlink.bindings)
}

java {
    toolchain {
        languageVersion.set(JavaLanguageVersion.of(22))
    }
}

application {
    mainClass.set("systems.zlink.samples.gamequest.client.Program")
    applicationDefaultJvmArgs = listOf("--enable-native-access=ALL-UNNAMED")
}
