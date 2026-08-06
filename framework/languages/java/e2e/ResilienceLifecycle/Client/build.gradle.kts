plugins {
    application
}

dependencies {
    implementation(project(":Shared"))
    implementation("systems.zlink:zlink-http-client:0.10.0")
}

application {
    applicationName = "resilience-lifecycle-client"
    mainClass.set("systems.zlink.e2e.resiliencelifecycle.client.Program")
    applicationDefaultJvmArgs = listOf("--enable-native-access=ALL-UNNAMED")
}
