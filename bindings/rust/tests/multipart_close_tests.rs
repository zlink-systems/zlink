use zlink::{Message, multipart_close};

#[test]
fn multipart_close_releases_every_payload_and_is_repeatable() {
    let mut parts = [
        Message::try_from(b"first").unwrap(),
        Message::try_from(b"second").unwrap(),
    ];
    multipart_close(&mut parts);
    assert!(parts.iter().all(|message| message.as_bytes().is_empty()));
    multipart_close(&mut parts);
    assert!(parts.iter().all(|message| message.as_bytes().is_empty()));
}
