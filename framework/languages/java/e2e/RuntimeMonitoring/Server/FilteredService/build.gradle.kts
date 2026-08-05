plugins {
    application
}

dependencies {
    implementation(project(":Shared"))
    implementation(project(":Server:Service"))
    implementation("org.springframework.boot:spring-boot-starter:3.5.14")
}

application {
    applicationName = "runtime-monitoring-filtered-service"
    mainClass.set("systems.zlink.e2e.runtimemonitoring.filteredservice.Program")
    applicationDefaultJvmArgs = listOf("--enable-native-access=ALL-UNNAMED")
}
