package systems.zlink.quickstart.shared

// Request/reply contract shared by the server and client processes.
data class Hello(val name: String)

data class Greeting(val text: String)
