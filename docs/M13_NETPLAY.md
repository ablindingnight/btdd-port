# M13: online 2-player (rollback netplay), design (2026-11-24)

Goal: two people play the 2-player game over the internet (host = player 1, guest = player 2) with no input lag felt
locally, on our deterministic core.

## Choice: own rollback implementation in C, GGPO's design
| | GGPO SDK (github.com/pond3r/ggpo) | own implementation |
|---|---|---|
| licence | MIT (fine) | ours (MIT) |
| language / build | C++ with a C API; Visual Studio project, CMake port exists | C11, our CMake, all three compilers |
| platforms | Windows (Winsock, `timeGetTime`); Linux/macOS only in forks | portable core; sockets behind a small platform layer |
| network layer | its own UDP stack, built in | pluggable transport: real UDP or a simulated link (latency, jitter, loss) for tests |
| testing | sync-test mode (rollback every frame, no network) | in-process two-peer test over the simulated link, the whole 2P TAS as a gate |
| fit | callbacks (save/load/advance) map onto our core | our core already restores exactly (M8: 78 KB state, rewind and state gates) |
| size | ~10k lines to vendor and maintain | ~1k lines |
GGPO's model is the right one (exchange only inputs, predict the remote input, roll back and re-simulate when a
prediction was wrong, optional input delay); its code is not needed: our save/restore is exact and cheap, two
players is the only case, and a transport we can simulate deterministically gives a strong automated test. So the
port implements that model itself (credited to GGPO's design, no GGPO code).

## How a frame runs (`src/net/rollback.c`, no SDL, no sockets)
- Each peer runs the same core from power-on. Frame f uses pad 1 = host input, pad 2 = guest input.
- **Input delay** D (0-3 frames, default 1): the local input read now is for frame `now + D`. Each frame of delay
  hides one frame of network latency at the cost of one frame of input lag.
- **Send**: every frame, one packet with the local inputs of the last up to 32 unacknowledged frames (redundancy:
  a lost packet is covered by the next one) and the newest remote frame received (the ack).
- **Prediction**: a remote input not received yet is predicted as the last one received (repeat).
- **Save**: the machine state at the start of every frame goes into a ring (16 frames; `nes_state_capture`,
  ~0.05 ms each).
- **Rollback**: when a remote input arrives for a frame that already ran with a different prediction, the state of
  that frame is loaded and the frames up to now are re-simulated with the corrected inputs (at most 8 frames, about
  2-3 ms with the interpreter), then the current frame runs. Only the last re-simulated frame is drawn.
- **Stall**: if the local side is more than 8 frames ahead of the last confirmed remote input, it waits (no frame
  advances) instead of predicting further; the time-sync below keeps this rare.
- **Time sync**: both sides report their frame number; the side that is ahead skips one display frame every so often
  (GGPO's "frame advantage") so neither runs ahead permanently.
- **Audio**: only frames run for the first time produce sound; re-simulated frames are silent (their sound was
  already played with the predicted input; the difference is a few milliseconds of one channel at most).
- **Confirmed frames**: a frame is confirmed when both inputs are real (no prediction). Everything up to the last
  confirmed frame can never roll back again.

## Session check (handshake)
`HELLO` carries a protocol version and a SHA-1 over: ROM SHA-1, build version string, PPU model, CPU path
(interpreter or native), the enabled logic mods and cheats with their values, and the input delay setting is
agreed (host's value). Different hash: both sides show what differs (the HELLO also carries the readable summary)
and refuse. Display-only mods (widescreen, no flicker, shaders) may differ.

## Desync detection and resync
- Every 30 confirmed frames each side sends a CRC-32 of the RAM (and PPU/APU registers) after that frame.
- Mismatch: both sides show "desync at frame N", write both states to `..\Gamefiles\netplay\desync_<time>\`
  (local files only), and the host can offer **resync**: it sends its state of the last confirmed frame (the
  machine state: RAM, CHR-RAM, registers; no ROM data) in chunks; the guest loads it and both continue from there.
  Nothing is sent anywhere but the peer.

## Connection
- Direct IP: the host listens on a UDP port (default 7845), the guest enters `IP:port`. The host needs that port
  forwarded on its router (or both on the same LAN / a VPN such as Tailscale/ZeroTier, which needs no forwarding).
- Later (planned, not built): a small relay / NAT hole-punching rendezvous server so neither side forwards ports;
  the lobby in the launcher (M11).
- btdd_play settings menu: **Netplay** page: Host game (port), Join game (address), input delay, status line
  (connected / ping / rollback frames per second / desyncs).

## Security basics
- Every packet: fixed magic and version, exact size checks against the type, frame numbers within a window of the
  current frame, input bytes as plain button bits; anything else is dropped and counted.
- Only the peer's address (fixed after the handshake) is accepted; other senders are ignored.
- Rate limit: more than 200 packets per second from the peer are dropped.
- The peer never sends code, file names or paths; a resync state is checked (size, format version, layout
  fingerprint, same ROM SHA-1) like a save state before it is loaded.

## Tests (no second PC needed)
- `nettest`: two peers in one process over the simulated link (latency, jitter, loss, duplication, reordering) play
  the 2P TAS inputs split between them (pad 1 on the host, pad 2 on the guest). Pass: on both peers, the RAM after
  every confirmed frame equals the solo run's RAM for that frame, for the whole movie, at e.g. 100 ms ping +
  jitter + 5 % loss; prints rollbacks and stalls. In `tools/check_all.py` as a gate.
- Two `btdd_play` instances on localhost (real UDP) for the handshake, menu and audio.
