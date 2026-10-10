# Critical Mass Mesh Light – pulse test v8

This version is based on v7 and changes only the single-heartbeat envelope.

## v8 pulse change

- one pulse per heartbeat (no `dub`)
- no flat peak hold
- longer rise: 12% of the heartbeat interval
- longer fall: 26% of the heartbeat interval
- both ramps use cosine easing, with zero slope at the bottom and at the peak
- all 16 LEDs still use the same intensity at the same time

The intention is a flatter, softer up/down ramp and less of a flash-like look.

## Unchanged

Mesh, CBOR/Base64 protocol, touch behavior, modes, emotes, Färgmyller and beat schedules are unchanged from v7.
