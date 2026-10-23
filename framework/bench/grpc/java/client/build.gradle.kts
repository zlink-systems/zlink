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

val activeBoundaryTest by tasks.registering(JavaExec::class) {
    classpath = sourceSets.test.get().runtimeClasspath
    mainClass.set("systems.zlink.bench.withgrpc.client.ActiveBoundaryTest")
    dependsOn(tasks.testClasses)
}
tasks.check { dependsOn(activeBoundaryTest) }

val admissionProgressTest by tasks.registering(JavaExec::class) {
    classpath = sourceSets.test.get().runtimeClasspath
    mainClass.set("systems.zlink.bench.withgrpc.client.AdmissionProgressTest")
    dependsOn(tasks.testClasses)
}
tasks.check { dependsOn(admissionProgressTest) }

// These measurements are opt-in and never run as part of check.
val requestBackpressureMains = mapOf(
    "requestBackpressureMicrobench" to "RequestBackpressureProbe",
    "requestBackpressureCompare" to "RequestBackpressureCompare",
    "requestBackpressureAdapterCompare" to "RequestBackpressureCompare\$Adapter",
    "requestBackpressureAllocations" to "RequestBackpressureAllocations",
)
requestBackpressureMains.forEach { (taskName, entryPoint) ->
    tasks.register<JavaExec>(taskName) {
        description = "Run the public request backpressure measurement ($entryPoint)"
        classpath = sourceSets.test.get().runtimeClasspath
        mainClass.set("systems.zlink.bench.withgrpc.client.$entryPoint")
        dependsOn(tasks.testClasses)
        jvmArgs("--enable-native-access=ALL-UNNAMED")
        doFirst {
            val rounds = providers.gradleProperty("probeRounds").getOrElse("3")
            val windowMillis = providers.gradleProperty("probeWindowMillis").getOrElse("2000")
            val costDirectory = providers.gradleProperty("probeCostDirectory").orNull
            if (costDirectory != null && entryPoint in setOf("RequestBackpressureProbe", "RequestBackpressureCompare")) {
                file(costDirectory).mkdirs()
                jvmArgs("-XX:-UseTLAB", "-XX:-DoEscapeAnalysis", "-XX:FlightRecorderOptions=stackdepth=256")
            }
            when (entryPoint) {
                "RequestBackpressureProbe" -> {
                    args(rounds, windowMillis)
                    if (costDirectory != null) args(file(costDirectory).absolutePath)
                }
                "RequestBackpressureCompare" -> {
                    args(
                        providers.gradleProperty("probeBeforeClasspath").get(),
                        providers.gradleProperty("probeAfterClasspath").get(),
                        rounds,
                        windowMillis,
                    )
                    if (costDirectory != null) args(file(costDirectory).absolutePath)
                    if (providers.gradleProperty("probeIncludeBinding").getOrElse("true").toBoolean()) args("--binding")
                }
                "RequestBackpressureCompare\$Adapter" -> args(
                    providers.gradleProperty("probeBeforeClasspath").get(),
                    providers.gradleProperty("probeAfterClasspath").get(),
                    providers.gradleProperty("probeBatches").getOrElse("512"),
                    providers.gradleProperty("probeWidth").getOrElse("128"),
                    rounds,
                )
                "RequestBackpressureAllocations" -> args(providers.gradleProperty("probeRecording").get())
            }
        }
    }
}

val sourceLifecycleTest by tasks.registering(JavaExec::class) {
    classpath = sourceSets.test.get().runtimeClasspath
    mainClass.set("systems.zlink.bench.withgrpc.client.SourceLifecycleTest")
    dependsOn(tasks.testClasses)
}
tasks.check { dependsOn(sourceLifecycleTest) }
