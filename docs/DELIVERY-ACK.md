# Glowline delivery acknowledgment & liveness protocol — full spec

**This document is self-contained.** It assumes no prior context. Sections 1–3 are shared and
must be read by whoever builds either half. Section 4 is `glowline-app` work. Section 5 is
`glowline-wled` work. The two halves must agree exactly on Section 3.

Save a copy in each repo: `docs/DELIVERY-ACK.md`.

---

## 1. Background

A glow sent to a strip that had been unplugged — no clean disconnect, no close frame, nothing —
still reported `status: 'sent'` with no error, and `/admin` still showed the device online. Tracing
it back: `sendLocal` (`src/durable_objects/DeviceConnection.ts:131-137`) returns `{ delivered: true }`
the instant `socket.send()` doesn't synchronously throw. There is no application-level
acknowledgement of anything sent to a device, ever. `isOnline()`
(`DeviceConnection.ts:144-146`, `ctx.getWebSockets().length > 0`) has the identical blind spot for
the same reason: nothing tells the server a specific connection has actually died if the far end
never sends a close frame.

An incremental, this-repo-only pass already shipped ahead of this spec (referred to below as
"Phase 1"):

- `glows.status`'s `'delivered'` value was renamed to `'sent'` — it only ever meant "we queued a
  send," and keeping the old name would collide with a real `delivered` status once this spec's
  ack exists (`src/db/schema.ts`, `glows.status` enum).
- `glows.send` gates each per-device send on `isDeviceOnline()` before calling `sendToDevice`, so a
  device the server *already knows* is disconnected fails fast (`src/actions/glows.ts:115-133`).
- A new `devices.connected` boolean, written only on connection-state transitions (`fetch()`/
  `logConnect` → true, `webSocketClose` → false, both in `DeviceConnection.ts`), plus a self-healing
  sync in `alarm()` (`DeviceConnection.ts:308-325`) that corrects drift within one cycle. This lets
  the Friends tab show presence from one D1 query instead of a per-friend Durable Object call.

**None of that is a substitute for this spec.** Phase 1 makes the server *honest about what it
already knows*; it cannot make the server know something it was never told. A device that goes
silent without a close frame — exactly the case that started this whole investigation — still
shows as connected under Phase 1 alone, because nothing has changed about what the wire tells the
server. Closing that gap needs the device's own cooperation, which is what this document specifies.

### What already exists that this builds on

- The Hibernation API auto-response pair is already wired and currently unused:
  `ctx.setWebSocketAutoResponse(new WebSocketRequestResponsePair('ping', 'pong'))`
  (`DeviceConnection.ts:38-40`), with staleness read via
  `ctx.getWebSocketAutoResponseTimestamp(socket)` (`DeviceConnection.ts:303`) and eviction already
  implemented (`STALE_THRESHOLD_MS = 90_000`, `EVICTION_CLOSE_CODE = 4001`,
  `DeviceConnection.ts:17-18`, `:298-307`). Confirmed, not assumed: auto-response replies incur no
  wall-clock time and are not billed (Cloudflare's own Durable Objects pricing docs) — the
  heartbeat itself is free; the existing 60s alarm cadence is the actual cost driver (Section 2).
- `tools/simulator.js` already sends this exact app-level `"ping"` every 30s
  (`tools/simulator.js`, `PING_INTERVAL_MS`) — **ahead of real firmware**, which today only replies
  to server-initiated RFC 6455 control-frame pings (`DeviceConnection.ts:32-37`'s own comment). The
  simulator also already has a `--go-silent-after <ms>` mode that stops responding without closing
  the socket, for exercising exactly the failure case this spec targets.
- The DO's alarm is already a single chain multiplexing three duties (eviction, reminders,
  offline-alert) onto one `ctx.storage.setAlarm()` call, taking the earliest of several candidate
  wake times (`DeviceConnection.ts:259-286`, `:358-360`). This spec adds a fourth duty to the same
  chain, not a second alarm — Durable Objects only ever have one.
- `sendToDevice(env, deviceId, wledJson)` (`src/lib/sendToDevice.ts:4-12`) is the one seam between
  app code and the Durable Object; `isDeviceOnline()` (`sendToDevice.ts:14-18`) wraps `isOnline()`.
  Both need richer return values under this spec (Section 4).

---

## 2. Design decisions, and why

**Heartbeat reuses the existing auto-response mechanism — no new infrastructure.** The server side
is already built and idle (see above). The only missing piece is firmware sending the `"ping"` text
frame it was always meant to answer.

**Affordable alarm interval scales with fleet size, and 60s stays fine for now.** Each alarm firing
is treated as ~1 Durable Object request + 1 SQLite row write (Cloudflare's own billing docs are
not fully explicit on this specific unit, so this is a documented assumption, not a guarantee —
worth checking against real dashboard metrics once deployed). At the current 4-unit prototype
scale, 60s/device costs 1,440 firings/device/day — 5.8% of the 100,000/day Durable Object request
budget on Workers Free, using 4 devices. The formula for later: `interval_seconds ≥ 86,400 ×
devices ÷ target_budget`. Recommendation: **keep the existing 60s alarm interval and 90s stale
threshold as-is** — there's no reason to widen them at this project's actual scale, and doing so
only trades detection latency for headroom the fleet doesn't need yet.

**Server-initiated probe reuses the same wire mechanism as the heartbeat**, for `/admin`'s
re-probe (Section 4/5). Whether existing firmware's reply to a raw RFC 6455 control-frame ping is
sufficient, or an app-level probe/reply pair is needed instead, is an open question for whoever
implements Section 5 — record the answer here once confirmed, since it changes what Section 4's
probe code actually sends.

**Explicit ack (not a WLED state echo).** Rejected: treating WLED's own state-change push
(`docs/DEVICE-STATE-PUSH.md`, once shipped) as an implicit "it worked" signal. A state echo has no
correlation to any specific glow — a button press, an NFC tap, or a reminder firing would produce
an identical-looking message, and the server would have no way to tell "this proves glow X landed"
from "something unrelated changed." An explicit ack, correlated by message id, is unambiguous.

**The message id lives in an envelope *outside* `wledJson`, not inside it.** CLAUDE.md is explicit
that the app never writes LED/effect code — the usermod hands `wledJson` straight to WLED's own
state deserializer. Injecting a protocol field into that object risks WLED's deserializer rejecting
or misinterpreting an unrecognized key, and conflates "data for WLED" with "data for the
transport." Instead, the raw push becomes a field *of* a small outer envelope; WLED never sees
anything but the same `state` object it always has.

**Per-recipient delivery status lives in a new table, not a `glows.status` mutation.**
`glows.status` is documented as write-once (`schema.ts`, the `glows.status` column comment: "these
three never change after insert"). An ack that arrives after `glows.send` has already returned
would have to violate that invariant to update `status` directly. A new `glow_deliveries` table
(Section 4) carries the real per-device, post-hoc truth instead; `glows.status` keeps its existing,
narrower meaning ("did the fan-out attempt reach at least one device").

**Capabilities are advertised at connect time**, alongside the existing `fw`/`leds` query params
(`DeviceConnection.fetch`, `DeviceConnection.ts:93-105`). The server only expects heartbeats, probe
replies, or acks from a device that has said it supports them. This is what makes the rollout safe:
firmware predating this spec, or a device that's rolled back to an older build (`docs/OTA.md`),
simply doesn't advertise the capability and the server falls back to today's behavior for it —
no version gate, no migration step tied to this shipping.

**No retry queue for an unconfirmed/offline glow — fail immediately.** A glow to a device that
isn't confirmed reachable gets `outcome: 'failed'` right away. This was a deliberate product
decision, not a technical default: the alternative (queue-and-retry with a TTL once the device
reconnects) was considered and explicitly rejected in favor of keeping this materially smaller —
see Section 4's `glow_deliveries.outcome` values for what "failed" means precisely.

**The ack timeout is not a guessed constant — its value is deferred to real measurement**, taken
from Section 4's `/admin` re-probe (which logs round-trip time against actual hardware) and the
OTA bench test's observed timing, with headroom over the slowest of those. This document specifies
the *mechanism* (wait synchronously inside the action up to a bounded timeout, then fall back to
"unconfirmed"); the number itself is filled in once that data exists, not before.

---

## 3. The shared contract

Both halves must implement this identically.

### Capabilities handshake

Appended to the existing `/ws` connect query string, alongside `fw`/`leds`:

```
/ws?device=<id>&token=<t>&fw=<version>&leds=<n>&caps=heartbeat,probe,ack
```

`caps` is a comma-separated list of capability tokens this firmware build supports. Absent or
missing token ⇒ the server does not expect that behavior from this device and falls back to
today's Phase-1-only handling for it. Firmware predating this spec sends no `caps` param at all,
identical to how it predates `fw`/`leds` today.

### Heartbeat

Unchanged from what's already built: device sends the app-level text frame `ping` roughly every
30s over the existing connection; the server auto-responds `pong` without waking the Durable
Object. Recency is read via `ctx.getWebSocketAutoResponseTimestamp()`. Only expected from a device
that advertised `heartbeat` in `caps`.

### Server-initiated probe

```json
{"type":"probe","id":"<probeId>"}
```

Reply, sent promptly:

```json
{"type":"probe_reply","id":"<probeId>"}
```

`probeId` is server-generated per probe, opaque to the device — echoed back unchanged so the
server can match a reply to its request. Only sent to, and only expected from, a device that
advertised `probe` in `caps`.

### Glow push envelope

Outbound — replaces today's raw `socket.send(JSON.stringify(wledJson))`:

```json
{"v": 1, "id": "<glowMsgId>", "state": { /* wledJson, byte-identical to today's raw push */ }}
```

`state` is handed to WLED's deserializer exactly as `wledJson` always has been — the envelope
wraps it, it does not modify it. `glowMsgId` is server-generated per send attempt, unique per
`(glow, device)` pair (Section 4).

Inbound ack, success:

```json
{"type":"state_applied","id":"<glowMsgId>"}
```

Inbound ack, failure (the envelope itself didn't parse, or `state` couldn't be handed to WLED):

```json
{"type":"state_rejected","id":"<glowMsgId>","reason":"<short machine string>"}
```

**"Acked" means received, parsed, and handed to WLED's deserializer — not confirmed rendered.**
WLED itself is trusted to apply a state it accepted; this protocol has no visibility past that
boundary, matching every other place in this app's spec (CLAUDE.md) that treats WLED as the
authority on LED/effect behavior.

Only sent to, and only expected from, a device that advertised `ack` in `caps`. A device that
hasn't advertised `ack` continues to receive the raw, un-enveloped push exactly as today, and the
server does not wait for or expect a reply.

### Missed heartbeat, probe, or ack ⇒ dead socket

Unchanged eviction mechanism (`DeviceConnection.ts:298-307`), extended to also fire when a probe or
ack the server sent gets no reply within its timeout (Section 4): close with the existing
`EVICTION_CLOSE_CODE` (`4001`). `devices.connected` is set `false` by the existing `webSocketClose`
write (`DeviceConnection.ts:250`) — no separate write path, confirmed the eviction path already
routes through that same handler (Cloudflare's docs: `webSocketClose` "fires whenever a WebSocket
connection is closed, whether initiated by the client or server").

---

## 4. `glowline-app` — the server half

### Schema

New table `glow_deliveries`: `id`, `glowId` (FK → `glows.id`), `deviceId` (FK → `devices.id`),
`sentAt`, `ackedAt` (nullable), `outcome` (`'confirmed' | 'unconfirmed' | 'failed'` — see below).
One row per `(glow, device)` send attempt.

`outcome` values, precisely:

- `'failed'` — the device wasn't confirmed reachable at send time (today's `isDeviceOnline()` gate,
  `glows.ts:124`), or it advertised no `ack` capability and the server has no way to confirm it.
  No retry (Section 2's product decision).
- `'unconfirmed'` — the send was attempted to a device that advertised `ack`, but no
  `state_applied`/`state_rejected` arrived within the timeout. Distinct from `'failed'`: this means
  "we don't know," not "we know it didn't land." Whether a late-arriving ack after this point
  upgrades the row is an open question for whoever implements this — if yes, it needs a way for the
  UI to learn about it (a follow-up read on the friends/inbox view is the natural fit, reusing the
  same no-background-polling constraint `src/scripts/friendsPresence.ts` already established).
- `'confirmed'` — a `state_applied` ack arrived within the timeout. This is the only outcome that
  should ever be described to a user as "delivered."

### `DeviceConnection.ts` changes

- Constructor: parse `caps` from the connect URL (`fetch()`, alongside the existing `deviceId`/
  `fw`/`leds` reads, `DeviceConnection.ts:93-105`) and remember it on the socket attachment
  (`serializeAttachment`, same mechanism `deviceId` already uses, `DeviceConnection.ts:103`) so
  `webSocketMessage`/`alarm()` can check it later.
- Pending-ack tracking, two tiers:
  - **Fast path** (the common case: ack arrives while the request that sent the glow is still
    open): an in-memory `Map<glowMsgId, resolver>` on the live DO instance. The object is actively
    running for that RPC — not hibernating — so this needs no `ctx.storage` write.
  - **Slow path** (timeout fires, or an ack arrives after the request already returned): a
    `ctx.storage` entry, so the pending state survives hibernation between the timeout and whenever
    the alarm next sweeps for it. This is the DO's first use of `ctx.storage.put`/`.get`/`.delete`
    for anything other than the alarm timestamp itself — there's no existing pattern to follow here
    beyond the alarm calls already in `ensureAlarmChain()`/`alarm()`.
  - The multiplexed alarm (`DeviceConnection.ts:287-360`) gains a fourth duty: sweep expired pending
    acks (mark `glow_deliveries.outcome = 'unconfirmed'`, clear the `ctx.storage` entry). **Do not**
    schedule a new, earlier alarm per glow to do this — that's exactly the sub-60s-precision pattern
    this codebase already removed once (the old `pendingGlowRevert`, `DeviceConnection.ts:121-126`)
    and shouldn't reintroduce. The existing 60s cadence is enough; a pending ack just waits for the
    next regularly-scheduled tick.
- `webSocketMessage` (`DeviceConnection.ts:159-225`) gains branches for `type: 'state_applied'` and
  `type: 'state_rejected'`, resolving the matching pending-ack entry (fast or slow path) and writing
  the `glow_deliveries` row's `outcome`/`ackedAt`.
- `sendState`/`sendLocal` (`DeviceConnection.ts:131-142`) need a richer contract: given a device
  that advertised `ack`, wrap the payload in the envelope, register the pending ack, and either wait
  (bounded by the timeout from Section 2) or return immediately with `'unconfirmed'` semantics —
  exact shape is an implementation decision for whoever builds this, guided by Section 2's timeout
  discussion.
- Probe support for `/admin`'s re-probe: send `{"type":"probe","id":...}`, track the pending reply
  the same way as an ack (reusing the same tier-1/tier-2 mechanism, not a separate one), and report
  round-trip time back to the caller.

### `glows.send` changes

Reads per-device outcomes from `glow_deliveries` instead of the current `result.delivered` boolean
(`glows.ts:138-159`). `glows.status` (`'sent'`/`'held'`/`'failed'`) keeps meaning "did the fan-out
attempt reach at least one device" — a real, ack-confirmed `'delivered'` status becomes something
the *inbox/friends UI* derives by reading `glow_deliveries`, not something `glows.status` itself
gains as a fourth value (Section 2's write-once reasoning).

### `/admin` re-probe

`src/pages/admin/index.astro` already renders the device list from one batch D1 query
(`index.astro:29-49`) then probes each device in parallel via `Promise.all` for online status —
extend that same loop to send the new probe message per listed device instead of (or alongside)
`isDeviceOnline()`, and surface round-trip ms. No reply within the timeout ⇒ mark
`devices.connected = false` and close the socket (same `EVICTION_CLOSE_CODE`). Admin-only, so no
fleet-scaling cost concern (bounded by human clicks, not device count).

### Verification without hardware

`tools/simulator.js` needs matching support before any of this is testable at all — it already
sends the heartbeat ping (ahead of firmware); it needs to also reply to a probe and to an
enveloped glow push with `state_applied`/`state_rejected`, per the project's simulator-first rule.

---

## 5. `glowline-wled` — the firmware half

### Heartbeat

Send the app-level text frame `ping` every ~30s over the existing WebSocket connection — the
server side already exists and is idle (Section 1). This is the one piece that, on its own, already
unblocks Phase 1's stale-socket eviction from being a no-op.

### Capabilities advertisement

Append `&caps=heartbeat,probe,ack` (whichever of the three this build actually implements) to the
existing `/ws` connect URL, alongside the `fw`/`leds` params it already sends.

### Probe reply

On receiving `{"type":"probe","id":"<probeId>"}`, reply immediately with
`{"type":"probe_reply","id":"<probeId>"}`, echoing `id` unchanged. Confirm first whether the
existing reply to a raw RFC 6455 control-frame ping is sufficient for the server's needs, or
whether this app-level probe/reply is required in addition — see Section 2's open question; update
this section once resolved.

**Resolved (glowline-wled side):** this build implements the app-level `probe`/`probe_reply` pair
unconditionally, in addition to (not instead of) the existing raw RFC 6455 control-frame
ping/pong reply that was already there. The device makes no assumption about which one the server
actually uses for `/admin`'s re-probe — both are available. Whether the server's re-probe code ends
up relying on the raw control-frame pong alone or on this app-level pair is a `glowline-app`-side
decision and remains open there; it does not block or change anything on this side, since both
mechanisms are already implemented here regardless of which one the server picks.

### Glow envelope + ack

On receiving `{"v":1,"id":"<glowMsgId>","state":{...}}`: unwrap `state` and hand it to WLED's
existing state deserializer exactly as today's raw push already is. On success, reply
`{"type":"state_applied","id":"<glowMsgId>"}`. On any parse/apply failure, reply
`{"type":"state_rejected","id":"<glowMsgId>","reason":"<short string>"}` rather than staying
silent — a device that only ever failed silently is indistinguishable from one that stopped
receiving anything at all.

### One firmware release

Heartbeat, probe reply, ack, and capabilities advertisement ship together in a single
`glowline-wled` release/OTA cycle (`docs/OTA.md`), not staggered — there's no reason to split a
release across these, and staggering would leave the server unable to tell "doesn't support X yet"
from "supports X but it's broken" for longer than necessary.

---

## 6. Confirm before building

- [x] Section 2's open question resolved (glowline-wled side): firmware implements the app-level
      `probe`/`probe_reply` pair unconditionally, alongside the pre-existing raw RFC 6455
      control-frame ping/pong reply. Which one the server's `/admin` re-probe actually relies on is
      still an open decision on the `glowline-app` side — not blocking here, since firmware already
      supports both.
- [ ] The ack timeout value picked from real measured round-trip data (Section 4's `/admin`
      re-probe run against actual hardware, plus the OTA bench test), not a placeholder — see
      Section 2. (`glowline-app`-side work.)
- [ ] `tools/simulator.js` updated to fake heartbeat, probe reply, and glow ack **before** any of
      this touches real hardware, per the project's simulator-first rule. (`glowline-app`-side
      work — this repo has no simulator/test harness of its own.)
- [ ] Confirm whether a late `'unconfirmed'` → `'confirmed'` upgrade (an ack that arrives after the
      timeout already fired) is worth surfacing in the UI, and if so, design that follow-up read —
      left open in Section 4 deliberately, since it's a product call as much as a technical one.
      (`glowline-app`-side work.)

---

## Implementation status (glowline-wled)

Section 5 is implemented in `usermods/glowline/glowline.cpp`:

- `beginConnect()` appends `&caps=heartbeat,probe,ack` to the `/ws` connect URL.
- `checkLiveness()` sends the app-level `"ping"` text-frame heartbeat every `PING_INTERVAL_MS`
  (30s), alongside the pre-existing control-frame ping.
- `handleTextFrame()` (called from `finishFrame()`'s text-frame case, replacing the old
  unconditional `applyJsonState()` call) type-dispatches each inbound text frame: a `probe`
  message gets an immediate `probe_reply`; a `{"v":1,"id":...,"state":{...}}` envelope is unwrapped,
  handed to `deserializeState()`, and acked (`state_applied`) or nacked (`state_rejected`,
  with a short `reason`) by `id`; anything else (non-JSON text like the server's `"pong"` reply, or
  a legacy raw un-enveloped state push) falls back to the pre-existing raw-apply behavior.

Known limitation, deliberately not fixed here: `MAX_FRAME_PAYLOAD` (512 bytes) is a fixed inbound
frame buffer, and the new envelope adds ~40-60 bytes of overhead versus a raw push, shrinking the
usable budget for large state payloads before truncation. A truncated/corrupted envelope now
surfaces as an explicit `state_rejected` (bad JSON) instead of yesterday's silent
`applyJsonState()` no-op, so this is a net visibility improvement even unfixed — revisit the buffer
size only if real glows start getting rejected for this reason.

Not implemented here (server-side, `glowline-app`): the `glow_deliveries` table, `DeviceConnection.ts`
pending-ack tracking, `/admin` re-probe wiring, and `tools/simulator.js` support. See this repo's
own `docs/DELIVERY-ACK.md` note for the cross-repo boundary — that work belongs to the session that
owns `glowline-app`.
