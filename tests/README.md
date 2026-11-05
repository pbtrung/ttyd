# C backend regression checks

Build and run the tests on Linux:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Python 3 enables the local HTTP/WebSocket integration tests. They use the Python
standard library, start isolated servers on loopback with ephemeral ports, and
need no package installation. Cross builds skip host-executed tests. The portable
utility tests also run in Windows CI.

For address and undefined-behavior sanitizers:

```sh
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined'
cmake --build build-asan --parallel
ctest --test-dir build-asan --output-on-failure
```

The C unit tests run LeakSanitizer as well. The server integration fixture disables
leak reporting at process exit because PTY children and libwebsockets have separate
shutdown lifetimes; address and undefined-behavior failures remain fatal. This
suite is not a claim that every external-library or daemon shutdown allocation
has been leak-audited.

The regressions cover the audit findings:

- Empty complete WebSocket messages and malformed initialization/resize JSON are
  rejected without terminating the daemon. Empty initial fragments remain valid.
- Window dimensions require integer values in range; fragmented messages use
  geometric buffer growth and a 16 MiB per-message limit, including the command byte.
- Initial titles/preferences and credential token responses use correctly sized
  allocations instead of trusting truncated `snprintf` lengths.
- Pause/resume stops PTY reads; child exit waits for final output to drain on POSIX,
  including output held back by a paused connection.
- Repeated child exits and client disconnects preserve daemon health.
- Base paths must leave room for all endpoints; WebSocket routes require exact
  matches. Duplicate command/option strings do not confuse command discovery.
- Descriptor close-on-exec, no-data read recovery, and immediate write-error
  cleanup are exercised directly with libuv.
- Windows command-line quoting handles empty strings, spaces, embedded quotes,
  trailing backslashes and a 20,000-character argument without fixed stack buffers.
- Short/invalid signal names and invalid CLI ranges are handled safely.

Windows ConPTY lifecycle changes were reviewed statically on the Linux development
host. The portable quoting tests run locally; native Windows compilation and
utility tests are wired into the existing MSVC workflow.
