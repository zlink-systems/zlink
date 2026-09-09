plugins {
    application
}

fun sampleProject(name: String) = project("${path.substringBeforeLast(":", "")}:$name")

dependencies {
    implementation(sampleProject("Shared"))
    implementation(zlinkLibs.zlink.stream.connector)
    implementation(zlinkLibs.zlink.bindings)
}


application {
    mainClass.set("systems.zlink.samples.zoneworld.client.Program")
    applicationDefaultJvmArgs = listOf("--enable-native-access=ALL-UNNAMED")
}
