# matchbook

A limit order book and matching engine in C++20, with price-time priority,
differential testing against a reference implementation, and measured latency.

## Design

| Concern | Approach | Cost |
|---|---|---|
| Price levels | Flat array per side, indexed by tick | O(1) to reach any level |
| Best price | Three-level occupancy bitmap, scanned with `countr_zero` | O(1), three dependent loads |
| Orders within a level | Intrusive doubly-linked FIFO over a pre-allocated pool | O(1) append, O(1) unlink |
| Order lookup | `unordered_map<OrderId, slot>` | O(1) cancel and replace |
| Allocation | Free-list over a pre-sized pool, no per-order `new` | no allocation on the hot path |

Prices are integer ticks. There is no floating point anywhere in the matching
path, so a price either crosses or it does not, with no tolerance question.

A flat ladder reaches a known price for free but leaves open which price is
best. The book stacks three bitmaps to answer that: one bit per tick, one bit
per word of those, one per word again. A 262,144-tick band summarizes down to a
single word, so the best bid costs three loads and three bit scans, and the best
ask costs the same in the other direction. Cancel is what this buys: reaching
the level is arithmetic, unlinking from the FIFO is two pointer writes, and
clearing the tick touches at most three words, so nothing searches.

Bounding the band is the tradeoff. Venues bound prices anyway, through price
banding at CME or limit-up limit-down in US equities, so the book rejects an
order priced outside its band with `PriceOutsideBand`. A 65,536-tick band costs
2MB of levels and 16KB of bitmap.

Supported: limit and market orders, GTC, IOC and FOK, cancel, and cancel-replace.
Trades execute at the resting order's price, which honors the passive side's
limit. A replace re-enters as a new order and loses time priority, which
matches how exchanges treat a price or quantity change. A replace carrying a
price, quantity, or id the book refuses leaves the original order resting.

## Correctness

Unit tests cover each behavior, and the occupancy bitmap has its own randomized
test against `std::set`, since a wrong answer there corrupts the best price
without tripping anything else. The differential test in
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
| submit, resting | 27.6M ops/sec | 36ns | 22ns | 52ns | 113ns |
| submit, crossing | 7.7M ops/sec | 130ns | 112ns | 204ns | 411ns |
| cancel, random order | 5.8M ops/sec | 172ns | 174ns | 234ns | 460ns |

The table reports the slowest of three runs. p50 holds within a nanosecond or
two across runs while resting-submit throughput ranges up to 38M ops/sec, and
the worst single operation lands between 15us and 165us, which on an unpinned
laptop measures the operating system.

### What the ladder changed

Replacing `std::map` with the tick ladder moved resting submits from 17.1M to
28M ops/sec and cut p50 from 40ns to 22ns. Cancel gained less, 203ns to 174ns,
because a random cancel at a million resting orders spends its time missing
cache on the order pool, not finding the level. Crossing submits did not move:
they walk the FIFO and emit trades, which the ladder never touches.

### Measuring below the clock

That 22ns p50 sits below the clock. `steady_clock` on Apple silicon reads the
24MHz timebase, so it advances in 41.667ns steps. An earlier version of this
benchmark timed one operation at a time, and every figure it printed landed on a
multiple of 41.667ns: 42, 125, 208, 375, 458, 792. The 42ns p50 was one tick.

The benchmark now takes three passes over each workload:

- **Throughput** reads the clock twice per million operations.
- **The distribution** times batches of 32, putting the tick at 1.3ns per
  operation.
- **Stalls** time single operations, which resolves only what runs for
  microseconds.

Dropping the per-operation clock reads raised throughput from 12.2M to 17.1M
ops/sec and cut p99.9 from 1.04us to 129ns, all of it timer jitter. Pre-sizing
the pool and index through `expected_orders` removed the real tail, taking p99.9
from 630ns to 199ns and deleting a 4.1ms worst case while leaving p50 alone. An
earlier draft credited pre-sizing with a median win of 83ns to 42ns, which was
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
- The band is fixed at construction. A venue that re-centers its band intraday
  would need the ladder to slide underneath the resting orders, which this
  does not do.

## License

MIT
