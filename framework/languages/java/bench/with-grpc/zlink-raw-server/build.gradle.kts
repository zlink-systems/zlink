plugins { application }

dependencies {
    implementation(project(":shared"))
    implementation(zlinkLibs.zlink.bindings)
}

application {
    applicationName = "bench-zlink-raw-server"
    mainClass.set("systems.zlink.bench.withgrpc.rawserver.ZLinkRawBenchServer")
    applicationDefaultJvmArgs = listOf("--enable-native-access=ALL-UNNAMED")
}
