plugins {
    application
}

dependencies {
    implementation("systems.zlink:zlink-http-client:0.9.0")
    implementation("com.fasterxml.jackson.core:jackson-databind:2.17.2")
}

application {
    applicationName = "observability-ops-a5-client"
    mainClass.set("systems.zlink.e2e.observabilityops.a5.client.Program")
    applicationDefaultJvmArgs = listOf("--enable-native-access=ALL-UNNAMED")
}
