# Job: apply the reviewed spec revision E1–E8 (0.33.0 #1639)

Worktree: /root/worktree/zlink-3868t (branch spec/1639-owner-multi-drain; uncommitted draft already present)
Source of the edits: /root/jobs/fx/cx/v21/s033/specrev/sol-review.md, section "Exact replacement text", E1–E8.
Report: write /root/jobs/fx/cx/v21/s033/specfix/report.md (absolute path; nowhere else)
Rules: no sub-agents or other models (no Astra). Do not commit or push. Edit only spec files under
framework/doc/framework/common/spec/server/.

## Task
1. The worktree already has a draft (see `git diff`). Sol's line numbers refer to that draft. Apply E1–E8
   exactly as written, in both .ko.md and .en.md, replacing the draft text where the edit covers it.
   - E2 replaces the draft's new bold paragraph in 08 §4.3 together with the old gate-less paragraph.
   - E3 replaces the draft's sentence in 04 "준비된 owner 집합"/"The Set of Ready Owners" bullet.
   - E1 replaces the draft's sentence in 02 §2 (the "Handler turn은 다음 구간이다" paragraph addition):
     revert that paragraph to main's text if E1 makes it redundant, and say which you chose.
   - E6 changes both 02 §9 and 00-foundation/06-framework-api.
2. Check every anchor link you add resolves to an existing heading id in the target file (use the same
   slug rule the repo's doc link checker uses; find it under tools/ or scripts/ and run it on the changed files).
3. ko and en must say the same thing; keep surrounding headings and anchors.
4. Report: list each edit E1–E8 with file:line ranges in ko and en, link-check output, and anything you
   could not apply exactly.
