plugins {
    application
}

dependencies {
    implementation(project(":TopologySupport"))
    implementation(project(":Trigger"))
    implementation("systems.zlink:zlink-stream-connector:0.10.0")
    implementation("systems.zlink:zlink-http-client:0.10.0")
    implementation(zlinkLibs.zlink.bindings)
    implementation("com.fasterxml.jackson.core:jackson-databind:2.17.2")
}

application {
    applicationName = "observability-ops-client"
    mainClass.set("systems.zlink.e2e.observabilityops.client.Program")
    applicationDefaultJvmArgs = listOf("--enable-native-access=ALL-UNNAMED")
}
