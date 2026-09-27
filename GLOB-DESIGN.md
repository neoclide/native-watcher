# Raw glob path exclusions

## Scope

Replace regular-expression sources in native `ignoreGlobs` with raw glob
strings. The native matcher excludes paths during scanning and event handling.
Selecting which changes an upper-level consumer wants to receive is separate.

This change is confined to native-watcher. Direct binary consumers must migrate
their options together with the binary; old regex strings are not compatible.
No regex compatibility mode, negation, allowlist, or new public case option is
introduced. Existing subscription, event-kind, and rename contracts remain.

## Matching contract

- Public `ignore` accepts strings. RegExp and other non-string entries fail
  before starting a subscription.
- Ordinary relative and absolute paths retain their existing normalization.
  Nonempty exclusions inside the watch root are then passed as raw relative
  `ignoreGlobs`, including literal names without `*`. This makes literal and
  wildcard exclusions use the same component-by-component case policy.
  Root/ancestor/outside-root absolute exclusions retain the existing
  `ignorePaths` route. Strings containing `*` are root-relative patterns.
- `*` consumes zero or more characters in one path component, including dots.
- A component exactly equal to `**` consumes zero or more complete components.
  Embedded or longer star runs, such as `a**b` and `***`, are ordinary
  component-local stars.
- All other characters are literal. There are no escapes, character classes,
  brace expansions, comments, extglobs, or negation.
- `/` separates components. Windows also accepts `\` as a separator; on POSIX
  it is literal. Public event paths retain the platform's normal form.
  Raw globs retain leading, repeated, and trailing separators; these are not
  additional wildcards or an implicit request for path normalization.
- Match complete relative paths, not arbitrary substrings. Matching an ancestor
  excludes its whole subtree. In particular, `cache/**` matches `cache` itself,
  and `**/*.log` matches both `a.log` and `sub/a.log`.
- An empty pattern does not exclude the watched root.

## Shared C++ implementation

Keep `Glob` as the small matcher and retain raw-string equality/hash semantics
for subscription sharing. Parse stable pattern structure once, then use bounded
iterative matching rather than recursive wildcard enumeration. A path segment
cannot be consumed by `*` across a separator. Multiple globstars must not cause
exponential backtracking.

`Watcher::isIgnored()` remains the shared entry point for all backends. Platform
queries belong in shared C++ code with conditional compilation around system
APIs, not separate implementations in inotify, FSEvents, and Windows backends.

For each candidate component, try exact matching first. Only when folding could
make that component match is its parent directory's case behavior needed. This
keeps common exact matches and unrelated paths free of extra filesystem queries.
Any memoization lasts for the current match only: no persistent case cache,
rename invalidation state, or backend-specific cache maintenance is required.

## Case behavior and query failures

Determine the rule for the actual parent of each component, not just the watch
root. Nested directories or mounts can have different rules.

| Platform | Query |
| --- | --- |
| macOS | `getattrlist`, `ATTR_VOL_CAPABILITIES`, and the validity bit for `VOL_CAP_FMT_CASE_SENSITIVE` |
| Windows | Directory handle, `FileCaseSensitiveInfo`, `FILE_CS_FLAG_CASE_SENSITIVE_DIR` |
| Linux ext4 | Directory descriptor, `FS_IOC_GETFLAGS`, `FS_CASEFOLD_FL` |

Close every temporary descriptor/handle. Distinguish a known result from an
unsupported query or a parent that has already disappeared. Unknown behavior
allows exact matching only; it is not reported as a detected sensitive volume.
Unexpected substantive failures need explicit handling and context.

macOS and Windows use native Unicode comparison facilities. Do not lowercase
UTF-8 bytes or mutate the process-wide locale. The user explicitly chose ASCII
case folding only for Linux ext4 casefold directories: bytes outside ASCII
remain literal. No Unicode tables, locale machinery, or extra dependencies are
needed. Public path ignores inside the watch root and wildcard ignores respect
this same boundary. Do not extend the legacy absolute-path alias cache to Linux:
it cannot represent mixed component policies and would require invalidation.

Deletion filtering must continue to work after paths disappear. The existing
indexes exclude ignored entries; verify recursive deletion and rename paths do
not reintroduce events for entries that were never indexed.

## API and dependency migration

- Remove `picomatch`, `is-glob`, and RegExp conversion from the JS wrapper.
- Change the public TypeScript declaration to `ignore?: string[]`.
- Read and validate raw string patterns at the native boundary.
- Remove `<regex>`, `std::regex`, and regex-specific error handling. Retain
  backend/subscription error cleanup, which also handles non-regex failures.
- Update README with syntax, case behavior, and the direct-binary migration.
- Replace obsolete regex tests with string validation, raw glob, and lifecycle
  coverage; preserve unrelated resource-cleanup tests.

## Implementation and independent review

Terra owns implementation and focused regression tests. The parent owns this
design, independent diff review, additional checks, and remote platform runs.
Review every new helper, state field, and fallback for a concrete requirement.
Reject unrelated refactors or unneeded abstractions.

Validation covers component boundaries, zero-level globstar, literal punctuation,
dotfiles, UTF-8 preservation, raw native options, RegExp rejection, symlink roots,
subtree pruning, rename boundaries, create/update/delete, and case behavior.
Use real filesystem behavior to determine test expectations, and explicit event
barriers rather than new fixed sleeps. Special-volume tests must report skips.

Required runs: build, `npm run test`, `./scripts/test-linux.sh`, and
`./scripts/test-windows.sh`. A platform unavailable for execution remains an
explicit validation gap; another platform's pass does not replace it.

## Final review and evidence

Reviewed the wrapper-to-binding-to-shared-watcher path and all new production
state. The additions have these current purposes:

- Parsed pattern components avoid repeating stable parsing for every event.
  Rolling match rows prevent exponential wildcard backtracking.
- The shared case-query function isolates the three platform APIs. Its
  `Unknown` result is needed for disappeared parents and unsupported queries;
  treating those as insensitive could exclude distinct paths.
- The per-match query map avoids repeated system calls when globstars compare
  several pattern components against the same directory. It does not survive a
  match, so it needs no rename or filesystem-policy invalidation.
- UTF-8 component handling supports native Unicode comparison and preserves
  invalid POSIX filename bytes. Linux folding changes ASCII bytes only.
- Boundary validation rejects the removed RegExp protocol and NUL strings
  before any subscription work. The Linux flag definition supports the older
  kernel headers used by the existing build workflow.

Review corrections included preserving the legacy absolute-path alias logic,
preserving literal raw-glob separators, applying macOS folding to entire
components, handling invalid UTF-8 without consuming following literal bytes,
and removing an accidental transitive dependency on `<regex>` for exceptions.
No backend-specific matcher or persistent sensitivity cache was introduced.

Validation on 2026-09-28:

| Check | Result |
| --- | --- |
| macOS rebuild and `npm run test` | 62 passed, 23 platform skips |
| Final `./scripts/test-linux.sh` run | 60 passed, 25 platform skips |
| Independent matcher oracle, including separator boundaries | 33,120 pattern/path pairs passed |
| Focused ignore/case/rename tests on a case-sensitive HFSX volume | 24 passed, 6 skips |
| Real create/update/delete and rename filtering on insensitive APFS and sensitive HFSX | Passed using positive event barriers |
| Mixed parent-volume rules, macOS Unicode aliases, and invalid UTF-8 | Focused checks passed |
| Linux ASCII-only folding with mocked directory policy | ASCII folds; non-ASCII stays literal |
| Source scan and rebuilt macOS binary symbols | No native regex use found |
| `git diff --check` | Passed |
