# matchbook

A limit order book and matching engine in C++20, with price-time priority,
differential testing against a reference implementation, and measured latency.

## Design

| Concern | Approach | Cost |
|---|---|---|
| Price levels | `std::map` per side, ordered so `begin()` is always the best price | O(log P) to reach a new level, O(1) for the best |
| Orders within a level | Intrusive doubly-linked FIFO over a slab-allocated pool | O(1) append, O(1) unlink |
| Order lookup | `unordered_map<OrderId, slot>` | O(1) cancel and replace |
| Allocation | Free-list over a pre-sized pool, no per-order `new` | no allocation on the hot path |

Prices are integer ticks. There is no floating point anywhere in the matching
path, so a price either crosses or it does not, with no tolerance question.

Supported: limit and market orders, GTC, IOC and FOK, cancel, and cancel-replace.
Trades execute at the resting order's price, so the passive side's limit is
honoured. A replace re-enters as a new order and loses time priority, which
matches how exchanges treat a price or quantity change.

## Correctness

Unit tests cover the behaviours individually. The interesting part is
`tests/test_differential.cpp`, which runs random operation sequences through
both the real book and a deliberately naive reference model in
`tests/reference_book.hpp`, and compares the full event stream plus the depth at
every price after every single operation.

The reference is a flat vector scanned linearly for the best eligible order. It
is far too slow to use, and obviously correct by inspection, which is exactly
what makes it a useful oracle. A divergence is reported at the operation that
caused it rather than at the end of the run.

Current coverage: 210 seeds, including ten sequences of 5,000 operations each,
with zero divergences. Both suites also run clean under AddressSanitizer and
UndefinedBehaviorSanitizer in CI.

## Performance

Apple M3, single thread, release build, 1,000,000 operations per case:

| Operation | Throughput | p50 | p99 | p99.9 |
|---|---|---|---|---|
| submit, resting | 12.2M ops/sec | 42ns | 208ns | 1.04us |
| submit, crossing | 6.2M ops/sec | 125ns | 458ns | 792ns |
| cancel, random order | 2.6M ops/sec | 375ns | 792ns | 3.1us |

Percentiles are reported rather than the maximum. Across repeated runs p50 to
p99.9 are stable within a few percent, while the maximum swings between 30us
and 800us depending on when the scheduler preempts the process. On an unpinned
laptop the maximum measures the operating system, not the book.

### What the numbers cost to get

The first benchmark showed a 4.1ms worst case on submit. That was the order pool
reallocating and the index rehashing as the book grew past a million resting
orders, stalling one unlucky order while a million nodes were copied.

Pre-sizing both through the constructor's `expected_orders` parameter moved p50
from 83ns to 42ns and cut the worst case by more than two orders of magnitude.
The free list needed the same treatment for the cancel path, for the same reason.

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

Deliberately out of scope, and worth stating rather than leaving to be found:

- Single threaded. Concurrency in a matching engine is a design decision about
  the whole system, not a lock to add to a book.
- No self-trade prevention. A participant can trade with their own resting order.
- No iceberg or stop orders, no auctions, no market-data snapshot recovery.
- `std::map` was chosen over a flat price ladder for correctness at arbitrary
  prices. A ladder indexed by tick would remove the O(log P) level lookup and
  is the obvious next optimisation for a bounded price band.

## Licence

MIT
