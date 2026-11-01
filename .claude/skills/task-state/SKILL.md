---
name: task-state
description: Checkpoint regimen for .claude/TASK_STATE.md. Use at the start of any long-running or multi-step task, after a meaningful state change, before /compact or /clear, and when resuming after compaction.
---

# Persistent task state and context compaction

For every long-running or multi-step task, maintain
`.claude/TASK_STATE.md` as the persistent checkpoint for the current task.

The checkpoint must contain enough concrete information for a new session to
resume the task without relying on previous conversation history. Do not wait
until context compaction is imminent. Automatic compaction may occur before
there is another opportunity to update the file.

Create the checkpoint at the start of a long-running task. Update it in place
after a meaningful state change, including:

- completing, starting, blocking, or changing a plan step;
- modifying a related group of files;
- making or revising an important technical decision;
- completing a Debug build, Release build, test run, or benchmark;
- encountering a failure, blocker, or abandoned approach;
- before an intentional `/compact`, `/clear`, or session exit;
- before reading very large files or running commands expected to produce
  substantial output.

The checkpoint must record:

- the current objective, acceptance criteria, and user constraints;
- plan steps marked as not started, in progress, completed, or blocked;
- completed work and remaining work;
- modified, created, moved, or deleted files, including relevant symbols and
  reasons for the changes;
- important technical decisions and their rationale;
- failed approaches, relevant error excerpts, and why they should not be
  repeated without new evidence;
- the most recent Debug, Release, test, benchmark, and hardware-verification
  status, but only when actually executed;
- current blockers;
- exact ordered next actions, with the first action executable as written.

Keep the checkpoint concise. Do not copy complete files, large diffs, verbose
build logs, or conversation transcripts into it. Refer to repository paths,
symbols, commands, commits, and concise error excerpts instead.

After automatic or manual context compaction, or when resuming a task:

1. Read `.claude/TASK_STATE.md`.
2. Run `git status --short`.
3. Inspect the relevant diff.
4. Verify that the checkpoint matches the working tree.
5. Correct stale checkpoint information before continuing.
6. Continue from the first incomplete item under `Exact Next Actions`.
7. Do not repeat completed work or a recorded failed approach without new
   evidence.
8. Update the checkpoint after the next meaningful state change.

The working tree and actual command results are authoritative. Never claim
that a build, test, benchmark, or hardware check passed unless it was actually
executed successfully.
