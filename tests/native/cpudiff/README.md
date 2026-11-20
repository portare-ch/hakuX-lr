# CPU differential tests

The same 32-bit programs run on an x86 CPU and under FEX, and their results
are compared byte for byte. They find where the ARM64 backend (FEX) computes
something else than the x86-64 backend (KVM, the host's own CPU) does.

Each test is a loop that runs one instruction form over 2048 inputs (special
values, random bit patterns, values of ordinary size) and stores every result
in an output region, then halts. `gen.py` lists the forms: the reciprocal
estimates, 64-bit integers through the x87, float-to-integer conversions, x87
arithmetic at each precision setting, the transcendental functions, SSE
arithmetic at each rounding mode, MXCSR's status flags, and code that rewrites
itself.

## Running

On an x86-64 Linux host with binutils and `/dev/kvm`:

```
python3 tests/native/cpudiff/gen.py --out build/cpudiff/tests
cc -O2 -o build/cpudiff/cpudiff-ref tests/native/cpudiff/ref.c   # or the x86-64 build's cpudiff-ref
mkdir -p build/cpudiff/ref && build/cpudiff/cpudiff-ref build/cpudiff/tests build/cpudiff/ref
```

On ARM64 (the device, or an arm64 container under qemu-user), with `fexrun`
built from `tests/native/fexrun` and the `tests` directory copied over:

```
mkdir -p out && ./fexrun --base auto --dump out tests tests/manifest.txt
```

Then, on either machine:

```
python3 tests/native/cpudiff/compare.py build/cpudiff/tests build/cpudiff/ref out \
    --expect tests/native/cpudiff/known.txt --show 4
```

`known.txt` lists the differences that are known, as differing cases per
test. The comparison fails when a test differs in more cases than listed.
After a fix, write the list again with `--write` and commit it with the fix.

## Reading the result

- **The reference is the host's CPU**, not a Pentium III. Where the two differ
  by design, the test is tagged `vendor` and judged against a model of the
  Xbox's CPU instead: `RSQRTSS`/`RSQRTPS` and `RCPSS`/`RCPPS` against Intel's
  tables (`native/cpu/intel_rsqrt_table.inc`, `intel_rcp_table.inc`).
- **x87 tests run FEX as the core does**: the reduced (64-bit) x87 with
  precision control, and single-precision rounding switched on where the
  test selects it.
- **The `extended` and `transcendental` tests differ by design** under the
  reduced x87 (the core's `NC_FEX_X87=80` switch is the exact mode). They are
  in the list to show when that changes.
- **The host features FEX sees matter** (AFP, RPRES, FRINTTS). `known.txt`
  is from a core without AFP and RPRES (Cortex-A510/A710); a host with AFP
  differs in fewer cases and reports them as gains. Under qemu-user choose
  the CPU with `QEMU_CPU`.
