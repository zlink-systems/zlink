plugins {
    application
}

fun sampleProject(name: String) = project("${path.substringBeforeLast(":", "")}:$name")

dependencies {
    implementation(sampleProject("Shared"))
    implementation(sampleProject("Server:Configuration"))
    implementation(zlinkLibs.zlink.http.client.java)
    implementation(zlinkLibs.zlink.stream.connector)
    implementation(zlinkLibs.zlink.bindings)
}


application {
    mainClass.set("systems.zlink.samples.gamequest.client.Program")
    applicationDefaultJvmArgs = listOf("--enable-native-access=ALL-UNNAMED")
}
