#![cfg(unix)]

use std::io::{Read, Write};
use std::os::fd::AsRawFd;
use std::os::unix::net::UnixStream;

use zlink::{POLLIN, PollItem, poll};

#[test]
fn raw_poll_projects_readable_and_clears_old_readiness() {
    let (mut reader, mut writer) = UnixStream::pair().unwrap();
    let mut items = [PollItem {
        fd: reader.as_raw_fd(),
        events: POLLIN,
        revents: 0,
    }];
    writer.write_all(b"a").unwrap();
    assert_eq!(poll(&mut items, 500).unwrap(), 1);
    assert_eq!(items[0].revents, POLLIN);
    reader.read_exact(&mut [0]).unwrap();
    assert_eq!(poll(&mut items, 0).unwrap(), 0);
    assert_eq!(items[0].revents, 0);
}
