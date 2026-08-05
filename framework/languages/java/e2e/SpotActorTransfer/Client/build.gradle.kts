plugins {
    application
}

dependencies {
    implementation(project(":Shared"))
    implementation("systems.zlink:zlink-stream-connector:0.1.0-SNAPSHOT")
    implementation("systems.zlink:zlink-http-client:0.3.1")
    implementation(zlinkLibs.zlink.bindings)
    implementation("com.fasterxml.jackson.core:jackson-databind:2.17.2")
}

application {
    applicationName = "spot-actor-transfer-client"
    mainClass.set("systems.zlink.e2e.spotactortransfer.client.Program")
}
