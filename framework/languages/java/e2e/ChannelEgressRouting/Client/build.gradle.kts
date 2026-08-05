plugins {
    application
}

dependencies {
    implementation(project(":Shared"))
    implementation("systems.zlink:zlink-http-client:0.3.1")
}

application {
    applicationName = "channel-egress-client"
    mainClass.set("systems.zlink.e2e.channelegress.client.Program")
    applicationDefaultJvmArgs = listOf("--enable-native-access=ALL-UNNAMED")
}
