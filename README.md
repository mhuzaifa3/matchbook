# matchbook

A limit order book and matching engine in C++20, with price-time priority,
differential testing against a reference implementation, and measured latency.

## Design

| Concern | Approach | Cost |
|---|---|---|
| Price levels | `std::map` per side, ordered so `begin()` is always the best price | O(log P) to reach a new level, O(1) for the best |
| Orders within a level | Intrusive doubly-linked FIFO over a pre-allocated pool | O(1) append, O(1) unlink |
| Order lookup | `unordered_map<OrderId, slot>` | O(1) cancel and replace |
| Allocation | Free-list over a pre-sized pool, no per-order `new` | no allocation on the hot path |

Prices are integer ticks. There is no floating point anywhere in the matching
path, so a price either crosses or it does not, with no tolerance question.

Supported: limit and market orders, GTC, IOC and FOK, cancel, and cancel-replace.
Trades execute at the resting order's price, which honors the passive side's
limit. A replace re-enters as a new order and loses time priority, which
matches how exchanges treat a price or quantity change. A replace carrying a
price, quantity, or id the book refuses leaves the original order resting.

## Correctness

Unit tests cover each behavior. The differential test in
`tests/test_differential.cpp` runs random operation sequences through both the
real book and a naive reference model in `tests/reference_book.hpp`, comparing
the full event stream and the depth at every price after every operation. The
reference is a flat vector scanned end to end for the best eligible order: far
too slow to use, and correct by inspection, which is what makes it a useful
oracle.

Coverage is 210 seeds including ten sequences of 5,000 operations, with zero
divergences. Both suites run clean under AddressSanitizer and
UndefinedBehaviorSanitizer in CI.

### What the oracle cannot catch

A differential test tells you two implementations disagree. It says nothing
about which one is right. Both books used to destroy a resting order before
validating its replacement and then reject it, and they agreed at every step.
The generator drew replace prices from 95 to 105 and quantities from 1 to 20, so
it never asked for a replace the book should refuse.

Fixing it took three changes: correct the reference, widen the generator, and
write unit tests that state the rule. Only the last is independent of the
oracle.

## Performance

Apple M3, single thread, release build, 1,000,000 operations per case:

| Operation | Throughput | mean | p50 | p99 | p99.9 |
|---|---|---|---|---|---|
| submit, resting | 17.1M ops/sec | 59ns | 40ns | 74ns | 129ns |
| submit, crossing | 8.6M ops/sec | 117ns | 115ns | 164ns | 309ns |
| cancel, random order | 4.1M ops/sec | 243ns | 203ns | 273ns | 639ns |

Run to run p50 holds within a few nanoseconds and throughput swings about 20%
with the scheduler. The worst single operation lands between 18us and 70us,
which on an unpinned laptop measures the operating system.

### Measuring below the clock

`steady_clock` on Apple silicon reads the 24MHz timebase, so it advances in
41.667ns steps, and a submit costs about the same. An earlier version of this
benchmark timed one operation at a time, so every figure it printed landed on a
multiple of 41.667ns: 42, 125, 208, 375, 458, 792. The 42ns p50 was one tick.

The benchmark now takes three passes over each workload:

- **Throughput** reads the clock twice per million operations.
- **The distribution** times batches of 32, putting the tick at 1.3ns per
  operation.
- **Stalls** time single operations, which resolves only what runs for
  microseconds.

Dropping the per-operation clock reads raised submit throughput from 12.2M to
17.1M ops/sec and cut p99.9 from 1.04us to 129ns. That old tail was timer
jitter.

Pre-sizing the pool and index through the constructor's `expected_orders`
parameter removed the real tail: p99.9 falls from 630ns to 199ns and a 4.1ms
worst case disappears, while p50 sits at 42ns either way. An earlier draft of
this section credited pre-sizing with a median win of 83ns to 42ns, which was
the clock rounding two ticks to one.

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/bench_order_book
```

No external dependencies. The test harness is 60 lines in `tests/harness.hpp`,
so the build stays hermetic and CI needs no network.

## Limitations

Out of scope by choice:

- Single threaded. Concurrency in a matching engine is a design decision about
  the whole system, not a lock to add to a book.
- No self-trade prevention. A participant can trade with their own resting order.
- No iceberg or stop orders, no auctions, no market-data snapshot recovery.
- The book uses `std::map` for correctness at arbitrary prices. A ladder
  indexed by tick would remove the O(log P) level lookup and is the obvious
  next optimization for a bounded price band.

## License

MIT
