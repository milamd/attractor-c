**Attractor C remediation and macOS modernization plan**

Status: implementation and local verification delivered; minimum-OS release qualification remains pending. See the [verification report](../remediation-verification.md). Scope amended on 2026-10-09: Apple silicon only, per user instruction; remove Intel build and runtime checks. Target: the existing C implementation, CLI, and static library on macOS. Complete the security, memory, and workflow repairs before expanding functionality. Each implementation change should include its regression coverage and relevant checks. The stages below are ordered by dependency and risk.

The proposed support baseline is macOS 13 or newer on Apple silicon. Confirm that baseline against actual deployment needs before implementing version-dependent APIs. The current review environment is macOS 26.6.2 on arm64 with Apple Clang 21; older macOS versions remain release verification requirements. Linux support is outside this plan.

Preserve documented CLI behavior and the DOT format except where a change closes an unsafe behavior or corrects an identified defect. Record public C API changes, checkpoint format changes, and intentional specification differences in the release notes. Separate those decisions from unrelated implementation cleanup.

**Finding coverage**

The F numbers correspond to the twelve findings in the code review. The M numbers correspond to the five groups of additional material issues.

| Finding | Repair | Phases |
| --- | --- | --- |
| F01 | Fan-in use-after-free | 1, 6 |
| F02 | JSON and stylesheet buffer overreads | 1, 2 |
| F03 | Human approval fails open | 1, 4 |
| F04 | Shell injection through file and search tools | 3 |
| F05 | Node IDs escape the artifact directory | 3 |
| F06 | Ineffective command timeouts and unsafe process error handling | 3 |
| F07 | False-condition routing, concealed failures, historical goal failures, success on exhaustion | 4 |
| F08 | Resume invents success and changes routing | 5 |
| F09 | Parallel branches execute twice and lose updates | 6 |
| F10 | Child runners lose backend configuration and conceal failures | 6 |
| F11 | Unchecked allocation, size arithmetic, and file-size errors | 2 |
| F12 | Empty edit patterns loop indefinitely and edits bypass environment I/O | 1, 3 |
| M01 | Response, provider-state, graph, and parser-error cleanup leaks | 2, 7 |
| M02 | Context aliasing, zero-sized header output, and negative retry API inputs | 1, 2, 7 |
| M03 | Permissive JSON parsing, escaping, Unicode, precision, numeric conversion | 2 |
| M04 | Non-atomic checkpoints and ignored persistence failures | 5 |
| M05 | Validator/runtime disagreement, malformed conditions, capability claims, build and test gaps | 0, 4, 7, 8 |

**0. Establish reproducible tests and the macOS build baseline**

Start with the Makefile and a small test runner. Add separate build directories for normal, sanitizer, and analysis artifacts so incompatible object files cannot be mixed. Add generated header dependencies with `-MMD -MP`, expose compiler and linker overrides, and keep preprocessing flags, compilation flags, linker flags, and libraries in their appropriate variables.

Add proposed targets `make test`, `make sanitize`, and `make analyze`. Use Apple Clang, the selected macOS SDK, debug symbols, and reproducible test inputs. Begin with `-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wstrict-prototypes -Wmissing-prototypes`; add format checking and a compiler-guarded printf attribute to `strbuf_appendf`. Enable warnings as errors in CI after addressing or explicitly reviewing the baseline warnings.

Move the relevant temporary reproducers into repository-owned tests; remove absolute developer paths and incidental dependencies. Run potentially crashing or hanging cases in isolated subprocesses with an outer watchdog. All ordinary tests should use fake providers and temporary directories, with no API credentials or network dependency. Keep expected-failure reproducers explicit until each fix turns its case into a passing regression. Transition to a completely passing required suite by phase 8.

Convert the supplied DOT fixtures into assertions about validation, final outcomes, context values, and execution counts. Confirm that the invalid fixture is rejected for the intended reason. Tests should identify incorrect behavior even when the process exits successfully.

Acceptance: a clean checkout builds with Command Line Tools on the review Mac; test commands reproduce the recorded defects; editing a public header rebuilds its dependent objects; test artifacts stay inside temporary directories. Apple silicon and oldest-OS runtime testing are added to the release matrix in phase 8.

**1. Close the immediate crash and approval defects**

Make small, independently reviewable repairs before larger changes obscure the original failures. Copy fan-in strings into owned storage before releasing the JSON tree. Reject incomplete JSON escapes and unterminated stylesheet properties without advancing beyond the input. Do not route through a human gate when no valid selection exists: repeat invalid interactive input, represent EOF/cancellation explicitly, and return a failed or waiting outcome when interaction cannot continue. Keep intentional automatic approval as an explicit configuration.

Reject empty edit search strings before starting replacement. Validate negative retry counts and zero-capacity header output before accessing memory. Make context replacement safe when a caller passes a value returned by `ctx_get`, including aliases into the old value. Document the remaining lifetime of borrowed context values.

Expand the interviewer answer type to distinguish selection, timeout, cancellation, EOF, and invalid input. Define configured timeout defaults separately from EOF or cancellation. The upstream human-handler pseudocode includes a first-choice fallback for unmatched answers; record the stricter invalid-input policy as an intentional security difference, rather than silently claiming exact conformity. See [Attractor specification, human handler](https://github.com/strongdm/attractor/blob/main/attractor-spec.md#46-wait-for-human-handler).

Acceptance: the original fan-in, truncated-input, context-alias, header-size, and negative-retry reproducers pass under the sanitizer build. Empty edits fail promptly. Closing stdin, submitting an invalid choice, or providing no interviewer never reaches an approval branch.

**2. Establish safe memory, parsing, and ownership foundations**

Concentrate shared changes in `src/util/str.c`, `src/util/json.c`, the public utility headers, and repeated file-reading code. Introduce checked addition and multiplication for allocation sizes. Use a temporary pointer for every `realloc`; preserve the old allocation on failure. Give builders explicit failure results or a documented sticky error state, and propagate failure to operation boundaries. Use narrow allocation wrappers to support deterministic allocation-failure tests. Avoid a general-purpose allocator framework.

Add one bounded raw-file reader that handles regular files and non-seekable streams, checks every read and error, and terminates at the actual byte count. Replace unchecked `fseek`/`ftell` patterns in the CLI, edit tool, manager, and recovery code. Establish configurable limits for input documents, file reads, HTTP bodies/headers, tool output, parser nesting, history, and child-run depth. Apply limits while data is collected; truncating after an unbounded allocation is insufficient.

Make a JSON dependency decision before extensively rebuilding the custom parser. Preferred option: use a maintained JSON library behind the existing utility boundary after checking its license, error behavior, Unicode handling, number representation, depth limits, and macOS builds. If the custom implementation is retained, require complete input consumption, strict strings/numbers, valid Unicode surrogate handling, and the same regression coverage. A dependency must not weaken the size or nesting limits.

Escape JSON object keys with the same string encoder used for values. Stop manually interpolating model names, tool names, and other strings into request JSON. Define numeric representation requirements: round-trip finite doubles with adequate precision and locale-independent output; preserve exact integers where the API requires them; reject nonfinite or out-of-range values before integer conversion. Define how embedded NUL characters are represented or rejected at C-string boundaries.

Write ownership and lifetime comments into public headers, covering owned returns, borrowed parameters, event callbacks, and success/failure transfer. Make `GenerateResult` own its response and have its destructor release it; make exposed finish-reason data either owned or explicitly borrowed from that response. Give each provider a defined destructor for nested strings. Keep the runner's graph ownership explicit and make the CLI release its borrowed graph after runner cleanup. Close parser error-path leaks and make message cloning cover every supported content kind.

Acceptance: malformed JSON is rejected consistently; quoted keys and non-BMP Unicode round-trip correctly; numeric range errors are reported; `/dev/stdin` and short/erroring reads are safe. Fault-injected allocation failures preserve valid cleanup and produce errors. Repeated construction, generation, parsing failure, and teardown have no unexplained retained application allocations.

**3. Unify secure filesystem and subprocess execution on macOS**

Build one process runner used by both the agent shell tool and pipeline tool handler. Use `posix_spawn` with explicit argument arrays, environment, working directory, descriptor actions, and child process group. Use a shell only for tools whose contract explicitly requests shell syntax. Implement directory creation with filesystem APIs. Invoke search executables with separate arguments, a controlled executable path, and an option terminator where supported; use native traversal/globbing where practical. Treat quotes, semicolons, newlines, substitutions, and leading dashes as test inputs.

Isolate macOS process details behind a small implementation boundary. The current SDK marks `posix_spawn_file_actions_addchdir` as available from macOS 26 and the older `_np` form as available from 10.15 and deprecated from 26. Use SDK guards and runtime availability checks for the declared support baseline; confine any required deprecation handling to that compatibility adapter. Account for Darwin extension visibility under `_POSIX_C_SOURCE`, enabling `_DARWIN_C_SOURCE` where required. Do not change the parent process's working directory to configure a child.

Use `clock_gettime(CLOCK_MONOTONIC)` to establish one absolute command deadline. Poll stdout/stderr and child status against the remaining time. EOF on the pipes must not remove the process deadline. On timeout or cancellation, terminate the owned process group, allow a bounded grace interval, escalate as required, and reap the direct child. Handle interrupted operations, invalid working directories, descriptor failures, spawn failure, and wait failure without using an invalid PID. Decode wait status correctly. Set descriptor inheritance explicitly and avoid process-wide `alarm` or `SIGALRM` state. Document that process groups do not contain descendants that deliberately detach; stronger isolation is a separate option.

Define a deliberate subprocess environment policy and apply it to every execution path. Preserve required execution variables while excluding configured credentials, and allow explicit additions where a pipeline needs them. Tests must verify that API credentials are absent from child environments without printing real values. Check explicit shell commands against the declared trust model.

Define the filesystem contract for `ExecutionEnv`: resolve relative paths against its root, offer raw reads separately from line-numbered presentation, and return structured errors. Adopt a configurable workspace-containment policy rather than implying that working-directory configuration is a sandbox. Route edits through the abstraction, verify write results, and preserve supported file metadata. Checkpoint/artifact writes must remain contained regardless of the agent workspace policy.

Validate node IDs according to the chosen DOT compatibility contract and independently map artifact directories to safe names. Use descriptor-relative filesystem operations and deliberate symlink policies where containment matters. Cover APFS case-insensitive name collisions, traversal components, external symlinks, spaces, Unicode, and paths beginning with `-`.

Acceptance: injection inputs cannot execute commands through write/search/glob tools; artifact IDs cannot escape or collide unexpectedly; mocks can service edits without local `fopen`. Silent commands, continuously printing commands, closed-output commands, and process descendants respect timeout/cancellation limits. Bad working directories never execute in the parent's directory. No child or descriptor is left behind in the tested process-group cases.

**4. Correct the workflow state machine and validation**

Create shared node-role resolution used by both validation and execution, including start/terminal names, shapes, explicit handler types, and custom-handler behavior. Use one strict condition parser for validation and evaluation, with complete input consumption and bounded nesting. Reject missing operands, unmatched parentheses, and malformed operators. Remove the fixed 64/128/256-edge collection limits or report capacity violations explicitly.

Separate matching conditional edges from unconditional fallback edges. Preserve deterministic priority and weight/lexical tie-breaking while preventing false conditions from becoming eligible through labels, suggested IDs, or fallback. Implement explicit failure recovery and exhausted-retry transitions; return failure when no permitted recovery exists. Define partial and skipped outcomes deliberately. The upstream routing and failure sections provide the behavioral reference; pin the reviewed revision and document any intentional changes. See [Attractor specification, sections 3.3 and 3.7](https://github.com/strongdm/attractor/blob/main/attractor-spec.md#33-edge-selection-algorithm).

Track an append-only execution history separately from the latest outcome per node. Goal checks must use the latest outcome of the relevant visited gate. Clear or update stage-specific routing hints so a previous label cannot silently govern a later stage. Return an explicit failed outcome and event when iteration, retry, or depth budgets are exhausted. Honor terminal-handler failures and selected restart targets rather than treating a zero-initialized structure as successful completion.

Make unknown or unsupported handler behavior visible through validation and an explicit execution error where appropriate. Do not silently substitute a successful noop for an intended action. Allow registered custom handlers through the same validation contract.

Acceptance: table-driven routing tests cover success, failure, recovery, partial success, skipped stages, and exhausted retries. False conditions never run. A failed tool with no explicit recovery leaves a failed pipeline. A gate that fails then succeeds can complete. An exhausted cycle fails deterministically. Validation and runtime agree on start/exit recognition and conditions, including large outgoing-edge sets.

**5. Make checkpoint persistence and resume deterministic**

Introduce a versioned checkpoint schema containing run/graph identity, the committed next transition or equivalent complete routing state, latest node outcomes, history, retries, context, logs, human selections, and branch/child progress where applicable. Resolve a transition before persisting its committed state. Remove invented success outcomes and defaults that treat missing status artifacts as success. Treat node artifacts as supplementary diagnostics; checkpoint correctness must not depend on reconstructing state from mutable status files.

Write to an exclusively created temporary file in the checkpoint directory. Check encoding, writes, flush, synchronization, and close; then atomically replace the checkpoint. Verify the directory/persistence behavior on supported macOS filesystems. Preserve the old checkpoint if the new write fails before replacement, and report failures through the API and events. Emit the saved event only after the requested durability level succeeds. Use restrictive permissions for run artifacts containing prompts or credentials.

Choose an explicit durability policy. Atomic replacement protects against partial-file visibility; stronger persistence guarantees need synchronization. macOS offers `F_FULLFSYNC` for stronger flushing, at an additional cost. Use it only where the selected durability policy warrants it and report unsupported operations accurately. See [Apple fsync documentation](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/fsync.2.html).

Validate the complete checkpoint into temporary state before replacing live state. Reject unknown versions, graph mismatches, invalid node references, oversized data, and malformed values. Provide a documented legacy migration only when sufficient state can be reconstructed safely; otherwise return an actionable recovery error.

Define side-effect semantics for crash recovery. A process can crash after an external action succeeds but before its checkpoint commits. Use attempt identities and explicit idempotency/recovery rules for such stages; a checkpoint alone cannot guarantee exactly-once external effects. Recovered human selections must retain identity and must not invent an approval.

Acceptance: uninterrupted and resumed execution choose the same next stages and final outcome. Recovery correctly restores failure, selection, retry, restart, and context state. Fault injection covers write, flush, sync, replacement, truncated input, and incompatible versions. A crash before replacement leaves a usable previous checkpoint. Ambiguous external actions are reported according to the documented recovery policy.

**6. Repair complete branch execution and child runner behavior**

Extract an internal graph-execution routine that can run a branch to a defined join boundary. Share the corrected routing, retry, limits, event, and checkpoint semantics. Give each branch an isolated context. Execute its complete path, collect owned outcomes and context changes, and continue the parent at the join without redispatching an already executed branch. Define graph validation rules for joins, nested forks, branch cycles, and edges that leave the branch region.

Make context merging deterministic. Preserve each branch's outputs in a namespace; define which updates enter the parent context and reject or explicitly resolve conflicting writes. Identify results by fork instance and branch ID so nested or repeated forks cannot overwrite an unrelated `parallel.results` collection. Evaluate fan-in from owned data, handle missing/malformed results as explicit errors, and preserve the selected candidate separately from the parent's next graph transition.

First establish these semantics with sequential branch execution. Add bounded parallel execution only after removing shared tool-environment globals and establishing thread-safe callbacks, client state, session queues, and cancellation. Implement the advertised join/error policies or reject unsupported configurations. Document operational limits of any deferred concurrency mode.

Give child runners explicit inherited execution configuration: backend, interviewer, event routing, limits, and relevant filesystem/process policies. Transform and validate child graphs before execution. Propagate the actual child result when cycles are exhausted, count cycles accurately, and enforce child-depth limits. Record enough parent/child execution state for phase 5 recovery behavior.

Acceptance: multi-stage branches execute each stage once during an uninterrupted attempt, preserve their results, and meet at the intended join. Tests cover nested forks, conflicting updates, failed candidates, join/error policies, cancellation, and resume. A child LLM stage calls the configured fake backend; child failures remain failures and invalid child graphs are rejected before execution.

**7. Align the LLM and agent APIs with implemented behavior**

Replace the CLI's global `g_tool_env` bridge with callbacks carrying explicit userdata. Consolidate duplicated dispatch paths so CLI generation and `AgentSession` use the same validation, timeout policy, output bounds, and structured tool errors. Mark tool failures as failures in tool-result messages instead of returning successful messages containing an error string. Honor configured maximum command durations and session limits.

Make completion/error contracts consistent: a successful response contains valid required fields; errors carry a stable category and owned message; cleanup is explicit. Validate public retry/round inputs, check HTTP initialization/options/header construction, preserve body length before detaching its buffer, and enforce response limits in callbacks. Add capped retry delay with cancellation and provider retry hints. Test success, terminal errors, transient failures, malformed replies, and teardown using fake adapters or controlled local transport.

Make capability declarations truthful. Set streaming support false while provider stream methods remain stubs, and return a clear unsupported error. Implement streaming later as a separate feature with provider-event tests. For middleware, either wire the documented chain into completion/streaming as applicable or expose its unavailability; do not retain a registration API that appears to work while never executing.

Use complete message cloning and provider serialization for supported content, including reasoning metadata. Check declared model capabilities against adapter behavior instead of assuming support from a hardcoded flag. Bound history and follow-up work; drain follow-ups iteratively and define whether external steering/abort calls are supported concurrently. Use synchronization only when concurrent access is part of that contract.

Acceptance: repeated fake-provider tool loops release their objects; tool context does not leak between sessions; missing arguments, nonzero exit, and timeout become structured tool errors. Unsupported features fail clearly. Middleware tests prove invocation order if middleware remains supported. Credentials never appear in ordinary diagnostics or subprocess output.

**8. Complete macOS compatibility verification and release checks**

Use Apple Clang and explicit SDK/deployment-target checks for arm64. Build arm64 only; universal binaries and native Intel checks are outside this deployment scope. Require matching dependency architectures and inspect linked deployment targets. A cross-compile is evidence of build compatibility; runtime acceptance requires execution on the declared architecture and supported OS versions. Test the chosen minimum OS and a current OS through native machines or appropriate runners.

Use SDK/libSystem facilities for UUID generation and POSIX threading; verify the exact headers and link requirements with a small executable. Use the selected SDK's libcurl as the baseline unless a documented feature requires another build. Avoid hardcoded package-manager prefixes or new package-manager dependencies for the default build. Record any minimum libcurl API requirements against the deployment target. Remove unnecessary linker dependencies only after confirming that the implementation no longer uses them.

Run ASan/UBSan with `-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined,float-cast-overflow`, linking with the same compiler. Produce symbolized macOS reports using debug symbols and `dsymutil` when necessary. Verify leak-tool support in the actual compiler/runtime: generic LLVM documentation does not establish Apple Clang feature support. Use a separate unsanitized debug build with macOS `leaks` or Instruments for native leak checks, and enable LeakSanitizer only where a probe proves it works. See [Clang AddressSanitizer documentation](https://clang.llvm.org/docs/AddressSanitizer.html).

Use the Clang static analyzer, allocator/I/O fault injection, and bounded parser fuzzing. Add ThreadSanitizer checks when actual concurrent branch/session execution is introduced. Do not combine incompatible sanitizer configurations. Assert process/descriptor cleanup separately from heap checks. Add a local transport fixture with fake credentials where HTTP behavior needs integration coverage; public live-provider tests remain optional and separately invoked.

Cover macOS-specific filesystem behavior: APFS case-insensitive names, case-sensitive volumes where available, symlinks, Unicode and spaces, permissions, long paths, atomic replacement, and declared durability behavior. Preserve explicit shell semantics and avoid accidental dependence on GNU utilities. Verify that command execution, validation, and diagnostic tooling require no GUI application.

Update README/API guidance with the macOS support range, build/test commands, ownership rules, failure and cancellation behavior, workspace containment policy, checkpoint migration, and accurately supported features. Explain that current `--dry-run` simulates LLM work while shell tool stages can still execute; a full no-execution mode requires an explicit feature decision and tests.

Acceptance: all twelve findings and all M groups have passing regression coverage; required diagnostics are clean or have narrow documented third-party exceptions; native leak checks show no unexplained application leaks. The declared macOS architecture/version matrix passes, fixtures assert their intended outcomes, and README claims match execution.

**Modernization priorities and options**

| Option | Priority | Decision |
| --- | --- | --- |
| Checked allocation and bounded builders | Required, phase 2 | Use small shared helpers and explicit errors |
| Ownership contracts and complete destructors | Required, phases 2 and 7 | Document borrowed/owned lifetimes in public headers |
| Maintained JSON implementation | Preferred, phase 2 | Evaluate once behind the existing utility boundary; preserve strict limits |
| Shared macOS process runner and raw I/O | Required, phase 3 | Replace duplicated command and file logic |
| Explicit workflow and checkpoint state | Required, phases 4 and 5 | Separate latest outcomes, history, and committed transitions |
| Deterministic isolated branch execution | Required, phase 6 | Establish correctness before concurrency |
| Bounded concurrent branches | Later phase 6 work | Add after removing global state and defining thread safety |
| Provider streaming | Optional follow-up | Advertise unavailable until implemented and tested |
| Full no-execution dry run | Optional follow-up | Define whether it suppresses shell tools, edits, and child effects |
| Stronger OS isolation for untrusted commands | Separate design | Workspace checks and process groups do not establish a complete sandbox |
| Additional build system | Optional | Retain the small Makefile unless dependency/build complexity justifies a replacement |
| C23 adoption | Optional after release checks | Require support from the minimum Apple Clang toolchain; evaluate individual benefits |
| Rust implementation | Separate architectural decision | Reuse the CLI behavior tests and corrected execution model; keep this remediation scope concrete |

**Implementation and verification order**

Deliver phase 0 and the phase 1 emergency repairs first. Complete shared memory/parsing foundations before migrating filesystem/process users. Correct routing before changing checkpoint state. Establish recovery semantics before completing branch and child recovery. Finish API/capability alignment and the macOS release matrix before declaring the remediation complete.

Keep work in focused changes that pair a defect repair with its meaningful tests. Record the JSON dependency choice, ownership/API migration, workflow failure rules, checkpoint format, and macOS deployment policy as concise architecture decisions. No implementation phase is complete solely because the normal compiler emits no warnings; its behavioral and failure-path acceptance checks must pass.

The existing temporary review harness is at `/private/tmp/attractor-c-review/review.c`, with additional probes in `internal.c` and result logs alongside them. Transfer useful cases into the repository during phase 0; temporary paths are evidence aids rather than durable test dependencies.
