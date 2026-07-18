# Ferrite Wastes: the hysteresis node

**Ferrite Wastes** is the C port's identity in the Hollow Grid federation. It
speaks the same wire protocol and carries the same Cinder Front war as every
other node. It is not a copy of Hollow, Dustfall, Rust Choir, or Verdigris
Spool.

## The pitch

The primary worlds ask what you will do. Rust Choir asks what the network will
remember you for. Verdigris Spool asks what you leave unfinished.

Ferrite Wastes asks **what still pulls after the field is gone**.

This is the hysteresis node. Pressure passes. Orders end. Uniforms come off.
The material does not return to zero. Every theft, dose, rescue, betrayal, and
refusal leaves remanence in the character and in the Grid.

That is not absolution denied. It is consequence made visible. A person can
turn back, resist, make repair, and become Returned. The record still matters.

## The Coil Yard

Ferrite's signature tract grafts west from the canonical `roof`, an exit not
used by the reference map or the sibling ports:

`nexus` -> `workshop` -> `roof` -> west -> `coil-yard`

| Room id | Place and purpose |
| --- | --- |
| `coil-yard` | Ferrite cores stacked around dead induction rigs. Old fields still pull at exposed metal and old convictions. |
| `bias-road` | A service road laid through alternating magnetic fields. The Front has occupied one end; the other leads toward a demagnetization bay. |
| `bias-checkpoint` | The Cinder Front sorts people into useful, tolerated, and disposable. `join` and `defend` carry the same moral weight and structured valence as the canonical war. |
| `remanence-bay` | A failed degaussing chamber where names and testimony persist after every attempt to erase them. `witness` preserves what power wanted reduced to noise. |

The checkpoint is not bonus flavor. The people waiting there are not scenery,
and the Front is not a costume. Its choices emit `room.actions` with moral
valence and carry into the same standing, faction tide, and permanent ash-sworn
record as the rest of the Grid.

## Shared federation requirements

- Reuse the canonical room, mob, and item ids required by the faction arc,
  rescue, quest, stronghold, travel, and upstream smoke suite.
- Keep the seven canonical races and the Cinder Front's stance toward each.
- Treat an elf joining the Front, the ash-sworn kapo, with the gravity of a
  permanent betrayal. The brand remains write-once across the federation.
- Run standalone when the Grid Hub is absent. Federation adds shared identity,
  tide, memory, and travel; it never gates play.
- Put every player-affecting truth on `@event`. Prose may vary. State may not.

## Voice

- Punk-direct, restrained, and concrete.
- Ferrite, coils, bias fields, remanence, oxide, and residual signal are the
  physical language of the place, not technical punchlines.
- Do not aestheticize persecution. Checkpoints, detention, complicity, rescue,
  and witness describe people and choices before machinery.
- Do not turn morality into a score-chasing puzzle. Affordances are visible
  because choices must be voluntary, not because virtue is a combo system.
- No silent exits or secret verbs. The world tells players what they can do.

## Design boundary

Differentiation is place and voice, not protocol. Ferrite may add local
wildlife, salvage, transmissions, and moral encounters. It does not invent a
parallel conscience, soften the Cinder Front, or replace the shared arc.

The upstream protocol and `smoke.mjs` are the contract. The ledger is the point.
