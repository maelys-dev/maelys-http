# Codec fuzzing

Four entry points feed the socket-free codec: `fuzz_request`, `fuzz_response`,
`fuzz_chunked` and `fuzz_smuggling`. They are differential oracles rather than
crash detectors. Each parses the same input whole and then one octet at a time,
and aborts when any observable result diverges: the terminal result, the
framing verdict, the byte counts, the header and trailer counts, the status,
and a hash of the delivered body. `fuzz_smuggling` additionally asserts that a
rejection is sticky and consumes nothing afterwards.

`fuzz/corpus/<target>/` holds committed seeds carrying the structure each entry
point looks for, so a run spends its budget on the boundaries instead of
rediscovering that a request starts with a method. They are read-only: no
command writes to them.

## What runs

`make check` builds each entry point against the ordinary library and replays
the seeds once. It proves the entry points still compile and that no seed
regressed, nothing more.

`make sanitizers` replays the same seeds under ASan and UBSan. This is the
deepest gate a host without libFuzzer can run.

`make fuzz-libfuzzer` is the campaign: Clang libFuzzer with ASan and UBSan,
seeded from the committed corpus and writing everything it discovers to
`build/libfuzzer/corpus/<target>/`. libFuzzer saves new inputs into the first
corpus directory it is given, so that one is always in the build tree. The
budget is deliberately small by default and overridable:

```sh
make fuzz-libfuzzer FUZZ_RUNS=1000000
```

`FUZZ_MAX_LEN` bounds the generated input size, 65536 by default, which is the
default cumulative header budget of the parser.

## macOS

Apple Clang ships no `libclang_rt.fuzzer_osx.a`, so `make fuzz-libfuzzer` fails
to link there. The mutation-free gates still run: `make check` replays the
seeds and `make sanitizers` replays them under ASan and UBSan. Run the campaign
on Linux, or in a container with a Clang that carries the fuzzer runtime.

## Scope

These entry points cover the codec only, which is what parses untrusted bytes
without a socket. The client state machine, the resolver and the connectors are
covered by the ordinary tests instead, since they need transport behaviour that
a byte string cannot express on its own.
