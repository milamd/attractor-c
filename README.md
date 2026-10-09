# attractor-c

`attractor-c` is a C11 workflow engine for macOS on Apple silicon. It reads DOT files and runs coding agents. The agents use large language models (LLMs) from Anthropic, OpenAI, or Google Gemini.

The build uses arm64 and a minimum macOS version of 13.0. Tests passed on macOS 26.6.2. The macOS 13 tests are not complete. Intel builds are not available.

## Build, test, and run the release binary

Install Apple Command Line Tools. The build uses `xcrun` to find the macOS SDK and its libcurl. No other packages are necessary.

Run these commands from the project directory:

```sh
make BUILD=build/release
make BUILD=build/release test
./build/release/attractor --validate-only test/simple.dot
./build/release/attractor --dry-run --logs-dir ./attractor-run-example test/simple.dot
```

The default build uses `-O2` and includes debug information. The release binary is `build/release/attractor`. The build also copies `attractor` and `libattractor.a` to the project directory.

The tests must show `All selected regressions passed`. The example must show `Validation: OK` and `Status: SUCCESS`. The tests need local HTTP connections but no real API keys.

**WARNING:** `--dry-run` uses simulated LLM responses. Shell stages and child workflows can still run commands. Shell commands can change files outside the project directory. Use `--validate-only` to do a workflow check without execution.

## Use an LLM

Set the API key for your provider. Replace `YOUR_API_KEY` and `YOUR_MODEL` with your key and model name:

```sh
export OPENAI_API_KEY='YOUR_API_KEY'
./build/release/attractor --provider openai --model YOUR_MODEL test/simple.dot
```

For other providers, use `ANTHROPIC_API_KEY` or `GEMINI_API_KEY`. Without a provider key, the program gives a warning and uses simulated LLM responses.

To see all command options, run:

```sh
./build/release/attractor --help
```

The exit status is zero for success or partial success. An error gives a nonzero exit status.

## Continue a saved run

Use the same workflow and log directory:

```sh
./build/release/attractor --resume ./attractor-run-example/checkpoint.json \
  --logs-dir ./attractor-run-example test/simple.dot
```

A completed checkpoint returns its saved result. The program rejects old checkpoints and changed workflows. After an interruption, examine external actions before you start another run. A checkpoint cannot make sure that an external action runs only once.

## Limits and other checks

Branches and tools run in sequence. Sessions use one thread. The LLM client uses text and tool calls. It cannot use streaming, image input, audio input, or document input.

```sh
make sanitize  # Do tests with ASan and UBSan.
make analyze   # Run the Clang analyzer.
make ci        # Build with -Werror, do tests, and run the analyzer.
make leaks     # Do selected tests and a memory leak check.
```

Use a new `BUILD` directory when you change build options. Library users must rebuild after public header changes.

## Error information

For validation errors, examine the specified rule and node. For checkpoint errors, keep the checkpoint and output files. For memory leaks, read `build/debug/leaks.log`.

See the [verification report](docs/remediation-verification.md) for test results and limits. See the [release notes](docs/RELEASE_NOTES.md) for API changes. See the [design decisions](docs/decisions/2026-10-08-remediation.md) for recovery rules and differences from the [Attractor specification](https://github.com/strongdm/attractor).

## License

Apache 2.0. See [LICENSE](LICENSE).
