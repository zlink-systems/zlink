use super::{
    ConcurrentTasks, HEADER_SIZE, LatencyStats, MAX_BURST_PER_SENDER, MultiArgs, PHASE_ACTIVE,
    PHASE_LATENCY, PHASE_WARMUP, SETTLE_NS, Schedule, SenderRotation, encode_header,
    message_payload, now_ns, record_measurement_receive, reply_turn, wait_ms,
};
use zlink::{Message, POLLIN, PollEvent, Poller, Received, SendSubmission, SubmitResult};

pub struct SendPhase<'a> {
    pub end: u64,
    pub echo: bool,
    pub schedule: Option<&'a mut Schedule>,
}

impl SendPhase<'_> {
    fn submit_due(&self, now: u64) -> bool {
        now < self.end && self.schedule.as_ref().is_none_or(|s| s.due(now))
    }

    fn phase(&self, now: u64) -> u8 {
        match &self.schedule {
            None => PHASE_ACTIVE,
            Some(schedule) if now < schedule.epoch + SETTLE_NS => PHASE_WARMUP,
            Some(_) => PHASE_LATENCY,
        }
    }
}

pub fn run_send_phase(
    args: &MultiArgs,
    poller: &Poller,
    slots: usize,
    drain_ns: u64,
    mut phase: SendPhase<'_>,
    mut submit: impl FnMut(usize, Message) -> SendSubmission,
    mut receive: impl FnMut(usize, &mut Received) -> bool,
    stats: &mut LatencyStats,
) -> (u64, u64) {
    let mut tasks = ConcurrentTasks::new(slots);
    let mut pending_due = vec![0; slots];
    let mut events = vec![PollEvent::default(); slots.max(1)];
    let mut received = Received::empty();
    let mut accepted = 0;
    let mut sequence = 0;
    let mut rotation = SenderRotation::default();
    let mut replies = 0;
    let drain_end = phase.end + drain_ns;
    let end = phase.end;
    let paced = phase.schedule.is_some();
    let sample_start = phase
        .schedule
        .as_ref()
        .map(|schedule| schedule.epoch + SETTLE_NS);
    let mut paced_received = 0;
    let mut last_reply = phase.end;
    let mut advance = |phase: &mut SendPhase<'_>| {
        let mut progress = false;
        let mut visited = 0;
        while visited < slots {
            let now = now_ns();
            if !phase.submit_due(now) {
                break;
            }
            let slot = rotation.slot;
            if rotation.can_submit() && !tasks.is_pending(slot) {
                let mut message =
                    Message::with_size(args.msg_size.max(HEADER_SIZE)).expect("message");
                sequence += 1;
                encode_header(
                    message.data_mut(),
                    phase.phase(now),
                    args.msg_size as u32,
                    sequence,
                );
                let due = phase.schedule.as_mut().map_or(0, |s| s.reserve());
                let submission = submit(slot, message);
                rotation.submitted();
                progress = true;
                if submission.result == SubmitResult::Backpressured {
                    pending_due[slot] = due;
                    tasks.insert(slot, submission.admitted);
                } else {
                    let admitted = now_ns();
                    if let Some(schedule) = &mut phase.schedule {
                        schedule.admit(due, admitted);
                    } else if admitted < end {
                        accepted += 1;
                    }
                }
            } else {
                rotation.advance(slots);
                visited += 1;
            }
        }
        for (index, result) in tasks.poll_ready() {
            result.expect("send admission");
            let admitted = now_ns();
            if let Some(schedule) = &mut phase.schedule {
                schedule.admit(pending_due[index], admitted);
            } else if admitted < end {
                accepted += 1;
            }
            progress = true;
        }
        (progress, tasks.any_pending())
    };

    loop {
        let (progress, pending) = advance(&mut phase);
        let now = now_ns();
        let quiet_end = last_reply + 50_000_000;
        if now >= end && !pending && (!phase.echo || now >= quiet_end) {
            break;
        }
        assert!(now < drain_end, "send admission drain timed out");
        let next = if now >= end {
            if pending {
                drain_end
            } else {
                drain_end.min(quiet_end)
            }
        } else if let Some(schedule) = &phase.schedule {
            if schedule.due(now) {
                end
            } else {
                end.min(schedule.next())
            }
        } else {
            end
        };
        let wait = if progress { 0 } else { wait_ms(now, next) };
        let count = poller.wait(&mut events, wait).expect("poller wait");
        for event in &events[..count] {
            if event.revents & POLLIN == 0 || event.slot >= slots {
                continue;
            }
            for _ in 0..MAX_BURST_PER_SENDER {
                if !receive(event.slot, &mut received) {
                    break;
                }
                let now = now_ns();
                last_reply = last_reply.max(now);
                assert!(now < drain_end, "reply drain timed out");
                let data = message_payload(received.parts());
                paced_received += u64::from(record_measurement_receive(
                    data,
                    args.msg_size,
                    now,
                    end,
                    sample_start,
                    2.0,
                    stats,
                ));
                if paced && reply_turn(&mut replies) {
                    advance(&mut phase);
                }
            }
        }
    }
    (accepted, paced_received)
}
