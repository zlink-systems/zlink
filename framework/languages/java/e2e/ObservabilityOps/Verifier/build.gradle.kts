plugins {
    application
}

dependencies {
    implementation("com.fasterxml.jackson.core:jackson-databind:2.17.2")
    testImplementation(platform("org.junit:junit-bom:5.10.2"))
    testImplementation("org.junit.jupiter:junit-jupiter")
    testRuntimeOnly("org.junit.platform:junit-platform-launcher")
}

application {
    applicationName = "observability-ops-verifier"
    mainClass.set("systems.zlink.e2e.observabilityops.verifier.Program")
}

tasks.test {
    useJUnitPlatform()
}
