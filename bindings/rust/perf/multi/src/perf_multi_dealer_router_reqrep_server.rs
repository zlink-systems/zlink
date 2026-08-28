#[path = "perf_common.rs"]
mod common;
#[path = "perf_multi_socket_reqrep.rs"]
mod socket_reqrep;

fn main() {
    socket_reqrep::run_server(socket_reqrep::ReqRepConfig::dealer_router());
}
