# Persistence and batch evidence

The app uses one I/O owner thread for SQLite, file fingerprints and UTF-8 export.
The reader and inference worker do not share database connections. The I/O
mailbox is bounded to 64 entries; position and attempt writes coalesce without
dropping unrelated settings updates. Shutdown drains accepted state mutations.

`state.sqlite3` contains preferences, native file identities, reading positions
and user source overrides. `cache.sqlite3` contains only usable normally completed
translations and attempt manifests, with separate statuses for pending, running,
complete, partial, failed and replaced chunks. Partial generated text remains
session display data. Cache deletion does not remove source overrides.

File SHA-256 reads use 1 MiB slices of the same opened file object used by PDFium
or llama. Hash work yields to I/O messages and never gates first rendering or
generation. Signature reuse checks native identity, size and modification time.
Provisional attempt names include the native signature, allowing a forced attempt
to be found again if the process exits before hashing finishes. Global completion
cache reads before both hashes are final are restricted to the originating I/O
session. Attempt-owned completed leaves remain associated with their attempt.

Native provisional overrides, including reset tombstones, commit immediately.
Promotion checks the latest revision in a state transaction. A cache-side durable
promotion intent bridges the two independent database commits; startup replays
unfinished promotions before exposing an attempt. This prevents an interrupted
forced attempt from disappearing behind an older completed attempt if the process
ends between fingerprint and cache commits. Completed leaves retain their own
text in the manifest, so a newer forced result cannot silently replace the text
owned by an earlier attempt.

Schema 5 upgrades supported development schemas in a transaction. Future schemas
are preserved and use memory fallback. Structurally corrupt cache files are
backed up before recreation; state files are never automatically deleted. A
failed state destination also falls back to memory, with an unsaved indication.
Idle cache maintenance counts completion data and manifest data, evicts inactive
documents with their references, checkpoints WAL and incrementally reclaims free
pages. The active document stays pinned even beyond the 256 MiB evictable budget.

The batch run holds a cursor and terminal metadata, rather than all source text
or jobs. It prioritizes the current page, retains the physical-page cursor while
prioritized work runs, and counts Complete, NoText, Partial and Failed separately.
Partial and failed pages are not automatically retried within that run. There is
one active page attempt and at most one retained displayed page attempt. Saved
source text is fetched per page, with only the displayed/work pages retained.

Current-page export labels unfinished output. Completed-page export streams one
page at a time, checks the latest source override, and excludes partial attempts.
Both write a temporary sibling file and replace the destination after success;
a failed export preserves an existing destination. Settings restore after the
window appears, saved models load asynchronously, and documents do not reopen
automatically. Reading state uses a PDF point and a UTF-8 text offset, not native
control pixel positions.

On macOS arm64, the native app completed the original three-page P01 batch while
remaining on page 1: two completed pages, one no-text page, no partial or failed
pages. Editing its translation source to a sentence containing 7 sensors and a
network negation produced the corresponding Japanese text. After quitting and
restarting, the model loaded from its saved path, the recent-file entry reopened
P01, and the source editor restored the exact edited English sentence. These
native observations preceded the final schema-5 hardening; migration and crash
recovery are covered by the automated tests.

Fifteen CTest cases cover the implementation, including semantic-key changes,
forced resume, promotion replay, native-object replacement, edit/hash races,
schema recovery, pins and eviction, session cache boundaries, export safety and
shutdown durability. Large-document memory/latency, native long-text viewport
restoration and Windows/Intel behavior still need their recorded acceptance
checks. The active-document pin has been tested with one Store owner; concurrent
independent app instances are not yet an acceptance claim.

Windows text-anchor and viewport handling follows the public Rich Edit contract
for [EM_CHARFROMPOS](https://learn.microsoft.com/en-us/windows/win32/controls/em-charfrompos)
and [EM_GETFIRSTVISIBLELINE](https://learn.microsoft.com/en-us/windows/win32/controls/em-getfirstvisibleline).
It uses UTF-16 native indices and stores a UTF-8 byte offset; Windows execution
has not yet been verified.
