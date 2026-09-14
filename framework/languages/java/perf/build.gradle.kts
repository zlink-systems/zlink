plugins { application }
dependencies {
    implementation("systems.zlink:zlink-framework-core:0.10.0")
    implementation("systems.zlink:zlink-framework-spring-boot-starter:0.10.0")
    implementation("systems.zlink:zlink-framework-locations-redis:0.10.0")
    implementation("systems.zlink:zlink-stream-connector:0.10.0")
    implementation(zlinkLibs.zlink.bindings)
    implementation("org.springframework.boot:spring-boot-starter:3.5.14")
    implementation("com.fasterxml.jackson.core:jackson-databind:2.19.0")
}
application {
    applicationName = "zlink-framework-perf"
    mainClass.set("systems.zlink.perf.Program")
    applicationDefaultJvmArgs = listOf("--enable-native-access=ALL-UNNAMED")
}
tasks.processResources { from("../../../perf-contract/histogram-bounds-ns.json", "../../../perf-contract/metric-catalog.json") }
tasks.register<JavaExec>("contractCheck") {
    dependsOn(tasks.testClasses)
    classpath = sourceSets.test.get().runtimeClasspath
    mainClass.set("systems.zlink.perf.ContractCheck")
}
tasks.register<JavaExec>("sessionPatternCheck") {
    dependsOn(tasks.testClasses)
    classpath = sourceSets.test.get().runtimeClasspath
    mainClass.set("systems.zlink.perf.SessionPatternCheck")
    jvmArgs("--enable-native-access=ALL-UNNAMED")
}
