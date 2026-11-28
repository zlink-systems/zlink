use zlink::{Received, RecvResult, TopicMessage};

#[test]
fn empty_envelopes_report_invalid_state_and_invalid_argument_errno() {
    let received = Received::empty();
    let topic = TopicMessage::empty();
    for error in [
        received.first_part().err().unwrap(),
        topic.first_part().err().unwrap(),
    ] {
        assert_eq!(error.code(), RecvResult::InvalidState);
        assert_eq!(error.internal_errno(), libc::EINVAL);
    }
}
