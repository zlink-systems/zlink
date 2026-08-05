plugins {
    application
}

dependencies {
    implementation(project(":Shared"))
    implementation("systems.zlink:zlink-http-client:0.3.1")
    implementation("systems.zlink:zlink-framework-core:0.1.0-SNAPSHOT")
}

application {
    applicationName = "runtime-monitoring-client"
    mainClass.set("systems.zlink.e2e.runtimemonitoring.client.Program")
    applicationDefaultJvmArgs = listOf("--enable-native-access=ALL-UNNAMED")
}
