plugins {
    application
}

dependencies {
    implementation(project(":server-support"))
}

application {
    applicationName = "spot-server"
    mainClass.set("systems.zlink.framework.perf.servers.spot.Program")
}
