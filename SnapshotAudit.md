# Audio runtime checkpoint audit

This API checkpoints in-memory audio runtime state for the native fixed simulation
clock. It does not serialize SDL handles, and it does not claim full game-world or
streaming-decoder checkpoint support.

## Why audio belongs to the simulation

`Sounds/WavePlay.cpp` polls `CKWaveSound::IsPlaying` and activates `End Playing`.
Ballance's `All_Sound/Music_Manager` connects that completion to `Random` and
`Random Switch` nodes. CK's RNG is shared with particle emitters. Using device
queue exhaustion to drive the script makes slow motion, fast replay and restored
branches execute different random calls even when their saved seeds match.

For CKTimeManager's fixed-delta mode, a source's double-precision PCM-frame cursor
advances in `PreProcess` by the simulation delta and effective pitch. The fractional
frame is retained. Loops wrap and non-looped sources end on a simulation boundary.
`IsPlaying` and `GetPlayPosition` observe that state. Hardware queues only present
audio; queued bytes, a draining device or real elapsed time cannot end the script.
The legacy hardware clock remains for non-fixed mode. Switching between those
clock policies during an already-playing clip has not yet been qualified.

## Field inventory

| Owner | Captured runtime state | Regenerated or excluded |
| --- | --- | --- |
| Source | PCM bytes, wave format/type, write/play cursors, fractional simulation frame, effective pitch/gain, 2D/3D settings, playing/paused/looping, stereo mix coefficients | SDL stream/device handles, queued byte counters, queued mix cache, scratch mix buffer |
| CKWaveSound | State flags, attached entity, prior/current position and direction, final gain/fade duration/fade progress, buffer counters, decoded/played counts, duration, PCM format, saved 2D/3D settings | Decoder object is identity-validated, not copied; file name is identity-validated; CK base/behavior state belongs to the world snapshot |
| Manager | Listener ID and prior pose/velocity/basis, listener settings, buffer policy, ordered playing-wave list | Device/mutex handles; device gain is reapplied |
| Minion | Ordered instances, retained source/original-source handles, original wave/entity IDs, position/direction/history and minimum-delay timestamp | Minion wrapper allocation is recreated; source identity remains stable |

Each saved source is held by shared ownership. Release stops its presentation and
removes it from the live registry, but cannot recycle a checkpoint's handle.
Preparation allocates replacement streams, PCM and minion wrappers without
binding to the device or modifying the live source. Commit swaps resources into
the retained source identities, restores waves and ordered lists, then binds
presentation streams. Destroying an unused preparation does not change playback.

A saved wave is validated using CK object lifetime tokens, decoder identity and
file identity. A different wave set or manager/reset epoch is rejected. Full-world
restoration still needs its own transaction and destroyed-owner reconstruction;
this audio API does not make an otherwise partial world restore atomic.

## Deletion callbacks

Ordinary CK object deletion must not dispatch the derived `PostClearAll` reset.
Both pre-deletion and post-deletion callbacks explicitly call only the base
listener validation. Pre-deletion removes only affected playing-wave entries and
clears references to deleted wave/entity owners. This matters when PracticeTools
disposes its temporary parameter copies after an overwrite save.

## Explicit unsupported cases and remaining gates

- A live streaming source is rejected during capture. Reader/ring-buffer/decoder
  checkpoints need a lossless reader API; rounded millisecond seeks are not one.
- Added, removed or replaced CK wave owners/readers reject preparation. Rebuilding
  destroyed saved CK objects is a separate world-snapshot requirement.
- Presentation latency and audible continuity under arbitrary speed changes are
  not proven by logical-state equality or the dummy-device regression.
- Third-party opaque audio handles outside CK's saved parameters require their
  own declared ownership/state coverage.
- Fixed/hardware clock transitions, device-loss recovery, cross-platform builds,
  complete corpus parity and final application workflows remain separate gates.

The regression uses a real CK context and SDL dummy device. It covers device-time
independence, fades, fractional pitch/looping, pause/resume, repeated restores,
expired minion handles, replaced sources, abandoned preparations, unsupported
streaming rejection, ordinary deletion and reset-epoch rejection. Test results,
exact tested binaries and game replays are tracked in the workspace experiment
log; this document is not a blanket pass certificate.
