# Remediation verification — 2026-10-09

The October 8 plan was implemented in dependency order: build/reproducers, immediate defects, memory/JSON/ownership, filesystem/process execution, workflow routing, checkpoint recovery, branches/children, API alignment, then local release checks. The October 9 scope amendment excludes Intel and universal builds. Implementation and local checks are delivered; qualification on the proposed minimum macOS 13 remains open.

## Environment and results

Native host: macOS 26.6.2 (25G83), arm64; Apple Clang 21.0.0 (`clang-2100.3.34.2`), selected Command Line Tools SDK 27.0. Deployment flags are `-arch arm64 -mmacosx-version-min=13.0`. Tests use temporary directories, fake credentials/adapters and loopback HTTP; they contact no public provider.

| Check | Observed result |
| --- | --- |
| `make` | CLI and static library built with the selected SDK, without package-manager dependencies |
| `make ci` | Strict C11/warnings-as-errors build, all 37 regressions passed, Clang analyzer completed without diagnostics |
| `make sanitize` | All 37 passed under ASan, UBSan and float-cast-overflow; no sanitizer diagnostics |
| `make macos-arm64` | Native arm64 CLI/library/test build passed |
| `make leaks` | All 34 selected inline cases passed; `0 leaks for 0 total leaked bytes` |
| CLI validation / dry-run / completed resume | `test/simple.dot` reported `Validation: OK`, then `Status: SUCCESS` for both execution and resume |
| Mach-O inspection | arm64 executable; `LC_BUILD_VERSION` minimum 13.0, SDK 27.0 |
| Dynamic dependencies | SDK `/usr/lib/libcurl.4.dylib` and `/usr/lib/libSystem.B.dylib` only |
| UUID / pthread / curl executable probe | Compiled and ran using SDK headers and `-lcurl`, without separate UUID, pthread or math libraries; runtime libcurl 8.7.1 |
| LeakSanitizer compiler probe | Apple Clang rejected `-fsanitize=leak` for this arm64 target; no unsupported LSan configuration is enabled |
| Generated dependencies | Public header changes rebuilt dependent objects; `.d` files include the utility and public API headers |

The sanitizer target uses `-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined,float-cast-overflow` for compilation and matching sanitizer link flags. CI and sanitizer objects are separate from normal/debug objects. Analyzer diagnostics fail `make analyze`; a successful ordinary compile alone is not its success criterion.

The execution sandbox initially denied loopback binding and native process inspection. The same complete CI/sanitizer suites and native leak gate were rerun with the required permissions and passed. These are test-environment restrictions, not skipped HTTP assertions.

Native `leaks --atExit` stops inherited fork children at exit on this host, preventing their waiting parent from finishing. The leak subset explicitly skips `branch committed recovery / new run`, `checkpoint committed recovery`, and `local HTTP transport / provider contracts`. All three run in the complete ordinary and sanitizer suites. Spawned shell processes use the deliberate environment policy and are exercised in the leak subset. The gate requires both the selected suite's success marker and a zero-leak report, so a crash cannot masquerade as a clean scan.

The native tool reports that the process is not debuggable and limits memory contents it can show. It nevertheless scans allocations and reports leak counts. It found a DOT lexer error-string leak during remediation; after that ownership repair, the final scan reported zero leaks. This is evidence for the tested subset, not a claim that the skipped transport fixture received native leak coverage. The full generated report is `build/debug/leaks.log`.

## Finding coverage

The repository-owned assertions are in [test/regressions.c](../test/regressions.c). Every required case currently expects success; there are no retained expected failures.

| Finding | Passing regression evidence |
| --- | --- |
| F01 fan-in lifetime | `fan-in ownership`, complete branches and nested joins |
| F02 parser overreads | Truncated JSON/stylesheet, parser allocation failures, 2,000 deterministic bounded fuzz inputs |
| F03 approval fails open | Missing interviewer; console EOF/invalid answers; cancellation; timeout default distinguished from EOF |
| F04 filesystem shell injection | Native recursive search, quote/semicolon/substitution inputs, contained raw reads/writes; no shell in file/search operations |
| F05 artifact escapes/collisions | Encoded traversal/absolute/dash/Unicode/space/case-variant IDs; distinct contents and mode 0600 |
| F06 command timeout/error handling | Silent, closed-output, continuous-output and descendant commands; invalid cwd; exit 7; pre-signaled cancellation; environment allowlist |
| F07 workflow outcomes/routing | False conditions, explicit recovery, failure without recovery, latest gate outcomes, partial/skipped/retry cases, bounded cycles and 301 outgoing edges |
| F08 resume invents success | Committed recovery chooses the saved transition; completed resume; strict checkpoint validation; graph mismatch and ambiguous pending action rejection |
| F09 incomplete/duplicate branches | Full isolated paths, deterministic conflicts, nested forks, join policies, committed branch crash recovery and fresh-run isolation |
| F10 child configuration/failure loss | Fake backend inherited by child; actual child failure retained; graph validation and depth/cancellation bounds |
| F11 allocation/arithmetic/read failures | Checked size overflow, sticky builders, parser/message/session/provider/checkpoint allocation injection, bounded non-seekable reads and read errors |
| F12 edit loops/abstraction bypass | Empty edit rejection and mock raw-read/write execution |
| M01 cleanup | Repeated provider tool loops/construction/teardown, parser-error fuzzing, ASan/UBSan and native leak subset |
| M02 aliases/invalid sizes/retries | Context self/interior aliases, zero header capacity, negative retry rejection |
| M03 JSON correctness | Full-consumption rejection, strict number/string syntax, quoted keys, surrogate/non-BMP handling, exact parsed integer spelling, finite double round-trip and checked integer conversion |
| M04 checkpoint persistence | Write/flush/sync/replacement injection preserves the prior file before replacement; truncated/incompatible loads preserve live state; recovery fixtures |
| M05 validation/capabilities/build | Shared roles and condition grammar, custom-handler validation, large edge sets, fixture execution counts, unsupported streaming/options, middleware order and strict build targets |

Process cleanup has separate assertions: timeout/error/allocation-failure paths preserve the count of open descriptors in the test's low descriptor range, and `waitpid` confirms no unreaped direct children. This supplements heap diagnostics; process groups are not a containment mechanism for deliberately detached descendants.

The supplied DOT fixtures assert backend dispatch counts (simple 2, branching 3, conditions 1, parallel 3, styled 3). The invalid fixture is rejected with the intended missing-start diagnostic. Generation fixtures verify independent callback userdata, explicit tool-error flags, duration clamping, middleware entry/exit order, nullable descriptions and unsupported request control rejection.

Filesystem checks cover symlinks, containment, case-variant artifact names, Unicode/spaces, restrictive artifact permissions and synchronized atomic replacement. Current-host volume inspection reports APFS. Checkpoints select `IO_SYNC` (file and directory `fsync`); the optional `IO_FULL_SYNC` utility policy is separate and does not establish a tested power-loss guarantee.

## Release qualification still required

Run the complete ordinary/sanitizer suite and appropriate native leak checks on an arm64 macOS 13 runner before claiming the full proposed support range. Compiling with minimum deployment target 13.0 does not substitute for that runtime test. This session had no such runner.

Case-sensitive-volume testing remains a matrix check when such a volume is available. No case-sensitive volume or power-loss testing was provisioned here. Live provider/account compatibility is optional and was not tested. Concurrent branches/session steering, streaming and a full no-execution dry run remain explicitly deferred features, rather than implemented capabilities requiring release acceptance.

API/checkpoint migration and intentional specification differences are recorded in [release notes](RELEASE_NOTES.md) and the [decisions](decisions/2026-10-08-remediation.md). The README reports the tested host and pending minimum-OS qualification explicitly.
