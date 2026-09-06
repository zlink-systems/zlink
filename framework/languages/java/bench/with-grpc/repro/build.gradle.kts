plugins { application }

dependencies {
    implementation(zlinkLibs.zlink.bindings)
}

application {
    applicationName = "bench-repro"
    mainClass.set("systems.zlink.bench.withgrpc.repro.OutstandingRequestRepro")
    applicationDefaultJvmArgs = listOf("--enable-native-access=ALL-UNNAMED")
}
