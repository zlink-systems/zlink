plugins {
    application
}

dependencies {
    implementation(project(":Shared"))
    implementation("systems.zlink:zlink-framework-core:0.10.0")
    implementation("systems.zlink:zlink-framework-spring-boot-starter:0.10.0")
    implementation("systems.zlink:zlink-framework-codec-protobuf:0.10.0")
    implementation("systems.zlink:zlink-framework-codec-msgpack:0.10.0")
    implementation(zlinkLibs.zlink.bindings)
    implementation("com.fasterxml.jackson.core:jackson-databind:2.17.2")
    implementation("com.google.protobuf:protobuf-java:4.30.2")
    implementation("org.springframework.boot:spring-boot-starter:3.5.14")
    implementation("org.springframework.boot:spring-boot-starter-web:3.5.14")
}

application {
    applicationName = "registration-codec-main"
    mainClass.set("systems.zlink.e2e.registrationcodec.main.Program")
    applicationDefaultJvmArgs = listOf("--enable-native-access=ALL-UNNAMED")
}
