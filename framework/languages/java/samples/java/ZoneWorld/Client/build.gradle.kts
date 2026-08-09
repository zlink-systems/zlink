plugins {
    application
}

fun sampleProject(name: String) = project("${path.substringBeforeLast(":", "")}:$name")

dependencies {
    implementation(sampleProject("Shared"))
    implementation("systems.zlink:zlink-stream-connector:0.10.0")
    implementation(zlinkLibs.zlink.bindings)
}

java {
    toolchain {
        languageVersion.set(JavaLanguageVersion.of(22))
    }
}

application {
    mainClass.set("systems.zlink.samples.zoneworld.client.Program")
    applicationDefaultJvmArgs = listOf("--enable-native-access=ALL-UNNAMED")
}
