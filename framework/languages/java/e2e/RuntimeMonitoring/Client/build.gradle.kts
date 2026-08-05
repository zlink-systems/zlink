plugins {
    application
}

dependencies {
    implementation(project(":Shared"))
    implementation("systems.zlink:zlink-http-client:0.9.0")
    implementation("systems.zlink:zlink-framework-core:0.9.0")
}

application {
    applicationName = "runtime-monitoring-client"
    mainClass.set("systems.zlink.e2e.runtimemonitoring.client.Program")
    applicationDefaultJvmArgs = listOf("--enable-native-access=ALL-UNNAMED")
}
