plugins { application }

dependencies {
    implementation(project(":shared"))
    // spec section 8.1: the framework row is zlink-framework-core with the protobuf codec
    // from zlink-framework-codec-protobuf, stood up through its public host, the
    // Spring Boot starter. No internal package is touched (G4).
    implementation("systems.zlink:zlink-framework-core:0.10.0")
    implementation("systems.zlink:zlink-framework-codec-protobuf:0.10.0")
    implementation("systems.zlink:zlink-framework-spring-boot-starter:0.10.0")
    implementation("org.springframework.boot:spring-boot-starter:3.5.14")
    implementation(zlinkLibs.zlink.bindings)
}

application {
    applicationName = "bench-zlink-framework-server"
    mainClass.set("systems.zlink.bench.withgrpc.frameworkserver.ZLinkFrameworkBenchServer")
    applicationDefaultJvmArgs = listOf("--enable-native-access=ALL-UNNAMED")
}
