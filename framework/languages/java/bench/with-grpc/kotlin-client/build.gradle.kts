import com.google.protobuf.gradle.id
import org.jetbrains.kotlin.gradle.tasks.KotlinCompile

// with-grpc local bench, kotlin row.
//
// The kotlin row deliberately owns only the CLIENT. It reuses this composite's :shared
// protobuf types and the java row's three server binaries, run on the kotlin port band
// of spec section 9. What separates `grpc-kotlin` from `grpc-java`, `zlink-kotlin` from
// `zlink-java` and `zlink-framework-kotlin` from `zlink-framework-java` is the
// client-facing API, and that is what this module implements.

plugins {
    application
    id("org.jetbrains.kotlin.jvm") version "2.2.21"
    id("com.google.protobuf") version "0.9.4"
}

kotlin {
    jvmToolchain(22)
}

dependencies {
    implementation(project(":shared"))
    // The harness itself: BenchOptions, BenchDrivers, StatsClient, ClientResources.
    // Boundary sampling (FB-013), the drain-confirmed settle (FB-008), peak_in_flight
    // and abandoned (FB-017), cell isolation, the bounded route-readiness probe and the
    // jvm_thread_cores instrument (FB-032) are inherited, not rewritten.
    implementation(project(":client"))
    implementation("io.grpc:grpc-netty-shaded:1.72.0")
    implementation("io.grpc:grpc-protobuf:1.72.0")
    implementation("io.grpc:grpc-stub:1.72.0")
    // spec section 8.1: kotlin uses the grpc-kotlin coroutine stub, because the ZLink side
    // uses the suspend interface of zlink-framework-kotlin. A blocking stub would tilt
    // the comparison toward kotlin.
    implementation("io.grpc:grpc-kotlin-stub:1.4.1")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-core:1.9.0")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-jdk8:1.9.0")
    implementation("systems.zlink:zlink-framework-core:0.10.0")
    implementation("systems.zlink:zlink-framework-kotlin:0.10.0")
    implementation("systems.zlink:zlink-framework-codec-protobuf:0.10.0")
    implementation("systems.zlink:zlink-framework-spring-boot-starter:0.10.0")
    implementation("org.springframework.boot:spring-boot-starter:3.5.14")
    implementation(zlinkLibs.zlink.bindings)
}

// The same bench.proto the java row generates from. A second copy would let the two
// rows drift apart, and then they could not be put next to each other.
(sourceSets["main"].extensions.getByName("proto") as SourceDirectorySet)
    .srcDir("$projectDir/../shared/src/main/proto")

protobuf {
    protoc { artifact = "com.google.protobuf:protoc:4.30.2" }
    plugins {
        id("grpckt") { artifact = "io.grpc:protoc-gen-grpc-kotlin:1.4.1:jdk8@jar" }
    }
    generateProtoTasks {
        all().forEach { task ->
            // Only the coroutine stub is generated here. BenchPayload and
            // BenchServiceGrpc come from :shared, so both rows carry the same
            // generated message classes rather than two copies of them.
            task.builtins { remove(getByName("java")) }
            task.plugins { id("grpckt") }
        }
    }
}

kotlin.sourceSets.named("main") {
    kotlin.srcDir(layout.buildDirectory.dir("generated/source/proto/main/grpckt"))
}

tasks.withType<KotlinCompile>().configureEach {
    dependsOn("generateProto")
}

application {
    applicationName = "bench-kotlin-client"
    mainClass.set("systems.zlink.bench.withgrpc.kotlinclient.BenchKotlinClientKt")
    applicationDefaultJvmArgs = listOf("--enable-native-access=ALL-UNNAMED")
}
