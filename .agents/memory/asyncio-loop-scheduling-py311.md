---
name: asyncio loop scheduling under Python 3.11
description: Do not call asyncio.get_event_loop() from sync code to schedule work; it raises RuntimeError in 3.11 when no loop runs in the thread. Capture the running loop lazily and fall back gracefully.
---

# Scheduling async work from sync callbacks (Python 3.11)

`asyncio.get_event_loop()` **raises `RuntimeError`** in Python 3.11 when
called from a thread with no running/current loop (it no longer auto-creates
one). Any sync method that schedules a coroutine via
`get_event_loop().call_soon_threadsafe(...)` will crash under unit tests or
worker-thread callers.

**Why:** Deprecation of implicit loop creation. Code that "worked" when a loop
happened to be current on the main thread breaks the moment it's called with
no running loop (e.g. a test that constructs a loop but never sets/runs it).

**How to apply:** In sync fire-and-forget dispatchers, prefer
`asyncio.get_running_loop()` inside a `try/except RuntimeError`, cache the
loop on first success (single-loop processes), and if no loop is available
log at debug and skip rather than raising. In the real runtime the sync
callback runs on the loop thread, so the loop is captured on first call and
`call_soon_threadsafe` dispatches normally. Optionally bind the loop
explicitly at startup to cover the first-call-from-worker-thread edge case.
