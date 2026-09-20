# Random-bot lifecycle and online level brackets

These are separate opt-in systems.

Level-bracket balancing affects only which eligible random bots are selected for
login. Configured ranges/percentages are validated against the core maximum level;
invalid syntax, overlap, count, range, or non-100 totals disable the feature with
an error. Deficits prioritize underrepresented brackets, then preserve existing
class/race balancing and fall back to the upstream selector. Online bots are never
kicked or de-levelled to satisfy a target.

Natural leveling performs the initial low-level setup once, then preserves earned
level and XP. It integrates with the existing `DisableRandomLevels` and
`randombotStartingLevel` behavior without changing their disabled defaults.

Persistent retirement applies only to random-account characters registered by
the existing architecture. It tracks max-level residence, selects gradually, and
excludes pending/retired/service identities from every login path. Character
finalization and the independent background estate are described in
`retirement-estate-service.md`; isolated build/crash gates are in
`lab02-lifecycle-validation.md`.

All new behavior defaults off. Retirement additionally has an immutable compiled
safety gate returning `false`; it cannot execute until the exact Linux/MariaDB
acceptance matrix passes and a separate reviewed change opens that gate.
