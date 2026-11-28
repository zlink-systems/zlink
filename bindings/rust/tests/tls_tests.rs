use std::time::Duration;

use std::process::Command;
use zlink::{ConfigResult, Context, Message, Received, RecvFlags};

#[test]
fn tls_configuration_rejects_nul_bytes() {
    let ctx = Context::new().unwrap();
    let socket = ctx.create_pair_socket().unwrap();
    assert_eq!(
        socket
            .set_tls_server("certificate\0.pem", "key.pem", false)
            .unwrap_err()
            .code(),
        ConfigResult::InvalidArgument
    );
    assert_eq!(
        socket
            .set_tls_client("certificate.pem", "localhost\0", false)
            .unwrap_err()
            .code(),
        ConfigResult::InvalidArgument
    );
}

#[test]
fn pair_round_trips_over_tls_with_explicit_trust() {
    if !zlink::has("tls") {
        eprintln!("Core was built without TLS");
        return;
    }
    struct CertificateDirectory(std::path::PathBuf);
    impl Drop for CertificateDirectory {
        fn drop(&mut self) {
            let _ = std::fs::remove_dir_all(&self.0);
        }
    }
    let directory = CertificateDirectory(
        std::env::temp_dir().join(format!("zlink-rust-tls-{}", std::process::id())),
    );
    std::fs::create_dir(&directory.0).unwrap();
    let cert = directory.0.join("cert.pem");
    let key = directory.0.join("key.pem");
    let openssl = Command::new("openssl")
        .args([
            "req",
            "-x509",
            "-newkey",
            "rsa:2048",
            "-nodes",
            "-days",
            "1",
            "-subj",
            "/CN=localhost",
            "-addext",
            "subjectAltName=DNS:localhost",
            "-keyout",
        ])
        .arg(&key)
        .arg("-out")
        .arg(&cert)
        .output()
        .unwrap();
    assert!(
        openssl.status.success(),
        "{}",
        String::from_utf8_lossy(&openssl.stderr)
    );

    let ctx = Context::new().unwrap();
    let server = ctx.create_pair_socket().unwrap();
    let client = ctx.create_pair_socket().unwrap();
    server
        .common_options()
        .set_receive_timeout(Some(Duration::from_millis(2000)))
        .unwrap();
    client
        .common_options()
        .set_send_timeout(Some(Duration::from_millis(2000)))
        .unwrap();
    server
        .set_tls_server(cert.to_str().unwrap(), key.to_str().unwrap(), false)
        .unwrap();
    client
        .set_tls_client(cert.to_str().unwrap(), "localhost", false)
        .unwrap();
    server.bind("tls://127.0.0.1:*").unwrap();
    client.connect(&server.last_endpoint().unwrap()).unwrap();
    client
        .send()
        .message(Message::try_from(b"secure").unwrap())
        .submit_sync()
        .unwrap();
    let mut received = Received::empty();
    assert!(server.recv(&mut received, RecvFlags::NONE).unwrap());
    assert_eq!(
        received.single_part_or_throw().unwrap().as_bytes(),
        b"secure"
    );
}
