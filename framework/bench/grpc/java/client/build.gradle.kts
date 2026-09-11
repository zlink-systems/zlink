import java.time.Duration

plugins { application }

dependencies {
    implementation(project(":shared"))
    implementation("io.grpc:grpc-netty-shaded:1.72.0")
    implementation("io.grpc:grpc-protobuf:1.72.0")
    implementation("io.grpc:grpc-stub:1.72.0")
    implementation("systems.zlink:zlink-framework-core:0.10.0")
    implementation("systems.zlink:zlink-framework-codec-protobuf:0.10.0")
    implementation("systems.zlink:zlink-framework-spring-boot-starter:0.10.0")
    implementation("org.springframework.boot:spring-boot-starter:3.5.14")
    implementation(zlinkLibs.zlink.bindings)
}

application {
    applicationName = "bench-client"
    mainClass.set("systems.zlink.bench.withgrpc.client.BenchClient")
    applicationDefaultJvmArgs = listOf("--enable-native-access=ALL-UNNAMED")
}

val rawProgressTest by tasks.registering(JavaExec::class) {
    description = "Verify raw request progress without a reply cap and across poller ownership changes"
    classpath = sourceSets.test.get().runtimeClasspath
    mainClass.set("systems.zlink.bench.withgrpc.client.RawProgressTest")
    jvmArgs("--enable-native-access=ALL-UNNAMED")
    dependsOn(tasks.testClasses)
    timeout.set(Duration.ofSeconds(40))
}
tasks.check { dependsOn(rawProgressTest) }
