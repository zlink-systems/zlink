plugins {
    application
}

dependencies {
    implementation(project(":Shared"))
    implementation("com.fasterxml.jackson.core:jackson-databind:2.17.2")
    implementation("systems.zlink:zlink-http-client:0.10.0")
}

application {
    applicationName = "store-failure-client"
    mainClass.set("systems.zlink.e2e.storefailure.client.Program")
}
