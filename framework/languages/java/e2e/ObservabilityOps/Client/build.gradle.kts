plugins {
    application
}

dependencies {
    implementation(project(":TopologySupport"))
    implementation(project(":Trigger"))
    implementation("systems.zlink:zlink-stream-connector:0.1.0-SNAPSHOT")
    implementation("systems.zlink:zlink-http-client:0.3.1")
    implementation(zlinkLibs.zlink.bindings)
    implementation("com.fasterxml.jackson.core:jackson-databind:2.17.2")
}

application {
    applicationName = "observability-ops-client"
    mainClass.set("systems.zlink.e2e.observabilityops.client.Program")
    applicationDefaultJvmArgs = listOf("--enable-native-access=ALL-UNNAMED")
}
