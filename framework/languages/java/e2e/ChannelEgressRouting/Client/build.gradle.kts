plugins {
    application
}

dependencies {
    implementation(project(":Shared"))
    implementation("systems.zlink:zlink-http-client:0.10.0")
    implementation("systems.zlink:zlink-stream-connector:0.10.0")
}

application {
    applicationName = "channel-egress-client"
    mainClass.set("systems.zlink.e2e.channelegress.client.Program")
    applicationDefaultJvmArgs = listOf("--enable-native-access=ALL-UNNAMED")
}
