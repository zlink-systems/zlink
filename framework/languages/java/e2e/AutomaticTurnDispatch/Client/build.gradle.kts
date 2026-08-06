plugins {
    application
}

dependencies {
    implementation(project(":Shared"))
    implementation("systems.zlink:zlink-http-client:0.10.0")
}

application {
    applicationName = "automatic-turn-dispatch-client"
mainClass.set("systems.zlink.e2e.automaticturn.client.Program")
    applicationDefaultJvmArgs = listOf("--enable-native-access=ALL-UNNAMED")
}
