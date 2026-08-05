plugins { application }

dependencies {
    implementation("systems.zlink:zlink-stream-connector:0.1.0-SNAPSHOT")
    implementation("systems.zlink:zlink-http-client:0.3.1")
    implementation(zlinkLibs.zlink.bindings)
    implementation("io.micrometer:micrometer-core:1.15.8")
    implementation("com.fasterxml.jackson.core:jackson-databind:2.17.2")
}

application {
    applicationName = "observability-ops-trigger"
    mainClass.set("systems.zlink.e2e.observabilityops.trigger.Program")
    applicationDefaultJvmArgs = listOf("--enable-native-access=ALL-UNNAMED")
}
