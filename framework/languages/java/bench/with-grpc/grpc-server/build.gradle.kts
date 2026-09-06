plugins { application }

dependencies {
    implementation(project(":shared"))
    implementation("io.grpc:grpc-netty-shaded:1.72.0")
    implementation("io.grpc:grpc-protobuf:1.72.0")
    implementation("io.grpc:grpc-stub:1.72.0")
}

application {
    applicationName = "bench-grpc-server"
    mainClass.set("systems.zlink.bench.withgrpc.grpcserver.GrpcBenchServer")
}
