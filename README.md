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
matches how exchanges treat a price or quantity change.

## Correctness

Unit tests cover the behaviors individually. The differential test in
`tests/test_differential.cpp` runs random operation sequences through both the
real book and a deliberately naive reference model in
`tests/reference_book.hpp`, and compares the full event stream plus the depth at
every price after every single operation.

The reference is a flat vector the test scans end to end for the best eligible
order. It is far too slow to use, and correct by inspection, which is what makes
it a useful oracle. The harness reports a divergence at the operation that
caused it.

Current coverage: 210 seeds, including ten sequences of 5,000 operations each,
with zero divergences. Both suites also run clean under AddressSanitizer and
UndefinedBehaviorSanitizer in CI.

## Performance

Apple M3, single thread, release build, 1,000,000 operations per case:

| Operation | Throughput | mean | p50 | p99 | p99.9 |
|---|---|---|---|---|---|
| submit, resting | 17.1M ops/sec | 59ns | 40ns | 74ns | 129ns |
| submit, crossing | 8.6M ops/sec | 117ns | 115ns | 164ns | 309ns |
| cancel, random order | 4.1M ops/sec | 243ns | 203ns | 273ns | 639ns |

### Measuring below the clock

`steady_clock` on Apple silicon reads the 24MHz timebase, so it advances in
41.667ns steps. Two adjacent reads with nothing between them report 42ns. A
submit costs about the same, so timing one operation at a time reports the
clock.

An earlier version of this benchmark did that, and every figure it printed
landed on a multiple of 41.667ns: 42, 125, 208, 375, 458, 792. The 42ns p50 was
one tick, which bounds the submit path somewhere between zero and 42ns and says
nothing else. The tails carried a 42ns error, and each
throughput figure included two clock reads per operation.

The benchmark now takes three passes over each workload:

- **Throughput** reads the clock twice per run of a million operations.
- **The distribution** times batches of 32 and divides, putting the tick at
  1.3ns per operation. Batch means flatten an isolated spike without losing it.
- **Stalls** time one operation at a time. Below a microsecond the tick swamps
  the result; above one it stops mattering.

The order stream is generated up front, so no random number generation lands
inside a timed region.

Run to run, p50 holds within a few nanoseconds. Throughput swings between 17M
and 22M ops/sec on submit depending on the scheduler. The worst single
operation lands between 18us and 70us, with an occasional 1.2ms on cancel. On an
unpinned laptop that number measures the operating system, not the book.

### What the numbers cost to get

The first benchmark showed a 4.1ms worst case on submit. That was the order pool
reallocating and the index rehashing as the book grew past a million resting
orders, stalling one unlucky order while a million nodes were copied. The free
list needed the same treatment for the cancel path.

Pre-sizing both through the constructor's `expected_orders` parameter cuts p99
from 156ns to 92ns and p99.9 from 630ns to 199ns, and removes the millisecond
worst case. It leaves p50 alone, at 42ns either way. An earlier draft of this
section claimed a median win of 83ns to 42ns, which was the clock rounding two
ticks down to one.

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
