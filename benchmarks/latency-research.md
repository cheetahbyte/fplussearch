# Further median query latency research

## Recommendation

Try a query-precomputed, two-position NEON prefilter in filename `find_ci`, followed by the existing `fold_eq` verifier. Keep typo scoring, candidate masks, job counts, and ranking unchanged. This is the smallest algorithmic experiment with no persistent index growth. Its benefit is unknown, especially for short names.

Before timing another change, validate the retained eight-jobs target on Chromium and compare complete responses. Linux socket medians improved approximately 24%; Chromium remains unverified. This research establishes no additional speedup.

The research itself ran no benchmarks. A subsequent first/last-byte NEON experiment used three rounds per variant on the identical Linux workload. Filename socket median was 0.072625 ms before and 0.074584 ms after (2.7% higher); p90 was 0.088959 and 0.097667 ms. The trial used query-byte broadcasts per `find_ci` call, not a query-precomputed descriptor. It was reverted because it established no gain. Search regression checks passed on the trial; the restored build, search checks, and disconnect test also passed. Exact and typo top-one accuracy matched; full response parity and Chromium performance were not measured. Raw results were saved to `/tmp/latency-neon-before.json` and `/tmp/latency-neon-after.json` and are temporary artifacts.

Every proposed benchmark invocation must use at most three rounds, including reversed-order comparisons. Earlier notes contain five-round and ten-round examples; do not reuse those settings.

## Current constraints

Reviewed `src/search.cpp`, `src/index.hpp`, `src/pool.hpp`, `src/content.cpp`, `src/server.cpp`, and the three existing optimization notes.

- Filename search scans masks in groups of 64 distinct names. It decodes only candidates and expands duplicate names into entry ranges.
- Filename `find_ci` vectorizes the first-byte search through `find_folded`, then verifies each candidate serially. Symbol `find_fold` already filters first and last bytes with NEON.
- Content `find_ci` already uses a first/last NEON filter. Content search already has folded trigram postings, follow-byte masks, dense bitsets, and smallest-first intersections. Adding another generic content trigram index duplicates existing work.
- `TopK` uses buffered selection, not a heap. Single-positive-token searches already publish a shared score floor and avoid scoring losing names while still counting matches.
- `Pool::run` allocates a batch, publishes it, wakes workers, and lets the caller claim jobs. Workers poll for 250 milliseconds after a batch. These facts identify possible costs, not their measured importance.
- Server `ms` excludes response serialization and transport. Socket latency includes those costs. One accepted connection starts a thread; that thread supports multiple newline-delimited requests.

### Semantic contract

Case folding maps only ASCII uppercase bytes to lowercase. Non-ASCII bytes compare literally; do not introduce Unicode normalization or Unicode case folding.

“Fuzzy” means contiguous substring matching, not subsequence matching. Clean scoring uses the first occurrence, not the highest-scoring occurrence. Typos apply only to fuzzy tokens of at least five bytes. They allow one substitution, insertion, deletion, or adjacent transposition, but never edit digits.

Prefix typo matching starts after an optional leading dot or immediately after a literal space, and preserves the first query byte. Whole-name typos can edit the first byte, but the actual `Token::fits` and start-mask gates still govern eligibility. Fixed typo text is limited to 128 bytes. Preserve those gates even where they seem unusual.

Multiple tokens can match separate ancestor directory components. Negated fuzzy tokens use clean contiguous matching rather than typo matching. Preserve exact totals, scores, folder/entry tie ordering, overlay deletion handling, and content smart-case behavior.

## Ranked experiments

### 1. Add a two-position filename substring prefilter

**Change:** Build a small matcher descriptor once per token. Select two distinct query offsets, compare both folded bytes in NEON lanes, and call `fold_eq` only for surviving starts. Start with first/last offsets; test a fixed rare-byte heuristic separately. Keep scalar handling for one-byte queries and short haystacks.

**Evidence:** The first-party [memchr packed-pair implementation](https://github.com/BurntSushi/memchr/blob/master/src/arch/all/packedpair/mod.rs) selects offsets using byte-frequency ranks and verifies possible matches. Its [AArch64 implementation](https://github.com/BurntSushi/memchr/blob/master/src/arch/aarch64/neon/packedpair.rs) uses 128-bit vectors. This establishes an implementation pattern, not a gain for this engine. Port the idea, not a Rust dependency.

**Code fit:** `search.cpp::find_ci` currently verifies every occurrence of the first byte. A second byte can reject those starts before verification. Reuse the existing lane-mask pattern, but not symbol scanning's padded-load assumptions.

**Tradeoff:** A few query-local bytes per token; no persistent memory. Descriptor setup and two loads can cost more than the current scan on short names. A frequency table adds a small read-only table; corpus-derived tables add build work and invalidate reproducibility unless frozen.

**Risks:** Return the earliest full match. Never report a cross-name match. Bound both loads to the actual view; decoded hex names live in scratch buffers. ASCII OR masks must depend on each selected query byte. High bytes and punctuation remain exact. Leave typo functions untouched.

**Small experiment:** Change only filename clean matching. Compare first-match offsets and complete ordered responses for repeated-byte needles, mixed case, high bytes, one-byte queries, hex names, and boundary lengths around 16 and 32 bytes. Record verifier calls and scanned bytes separately from timed runs. Compare Linux and Chromium in isolated processes, both orders, at most three rounds per invocation. Reject if aggregate medians or short-query classes regress consistently.

### 2. Prototype inverted filename grams with a conservative typo fallback

**Change:** Add folded byte-trigram postings over `(section, distinct-name ordinal)`, not entries. Intersect clean-query postings, then apply the existing name and entry predicates. Initially enable only a single positive non-typo token of at least three bytes. Fall back for fuzzy tokens accepting typos, short queries, negation-only queries, and complex ancestor matching.

**Evidence:** Official [SQLite trigram documentation](https://sqlite.org/fts5.html#the_trigram_tokenizer) demonstrates indexed substring retrieval and documents short-query fallback. Its grams use Unicode characters; this engine requires byte grams. SQLite is evidence for the approach, not a compatible replacement.

**Code fit:** Replace global `Scan::candidates` enumeration for eligible queries. Reuse content posting concepts, but do not attach filename entries to content documents. Resolving a name ordinal needs its block and entry-range start; current `candidates` derives entry starts by summing counts. Measure that lookup cost.

**Tradeoff:** If `P` is distinct `(gram, name)` pairs, uncompressed 32-bit name postings require `4P` bytes, plus the gram dictionary and offsets. This is a representation estimate, not a footprint prediction. Delta coding reduces storage but costs decoding. Dense bitsets cost approximately `N/8` bytes per gram for `N` names. Grams can exceed the existing eight-byte name masks substantially. Common grams, short queries, and random hex names can make retrieval unattractive.

**Risks:** Gram intersection does not prove adjacency or repeated-byte multiplicity; retain verification. Index each name independently, including decoded hex text. Deduplicate candidates before counting entries. Preserve overlay scanning and deletion filtering. For multiple tokens, intersecting filename postings loses entries whose missing terms match ancestors; retain the original path until a complete ancestor-aware plan exists.

**Typo extension:** Never intersect every original trigram for typo queries. An edit can destroy required grams. One conservative alternative is three disjoint query pieces, unioning retrieval for each piece and then running unchanged typo verification. A single substitution/deletion affects one piece; an insertion or adjacent transposition can disrupt at most two pieces, leaving one intact. This is a source-code-derived completeness argument, not a general edit-distance index claim. Require three pieces of at least three bytes, so use it only from nine bytes; otherwise fall back. Union these candidates with clean candidates. Broad piece postings may erase any benefit. Repeated grams make informal “shared gram count” thresholds unsafe unless multiplicity and transpositions are proved explicitly.

**Small experiment:** Build an in-memory sidecar only for eligible queries. Record `P`, sidecar bytes, build time, postings decoded, candidate names, and candidate-entry lookup time. Check recall against the mask-scan baseline before timing. Extend to typos only after adversarial parity checks for all four edits, piece boundaries, digits, first-byte edits, leading dots, and spaces. Use both datasets and orders, at most three rounds per invocation.

### 3. Measure content verification, then refine its posting/read boundary

**Change:** Instrument `ContentIndex::grep` around `eval`, candidate sorting, and `verify`; record posting bytes, candidate count, files read, and bytes read. If false-positive reads dominate, test removing only `eval`'s shortcut that skips large varint lists when at most four candidates remain. If allocation dominates, test bounded reader-owned reusable buffers, not a content cache.

**Evidence:** [SQLite's detail option](https://sqlite.org/fts5.html#the_detail_option) documents the storage/capability tradeoff from omitting positions. Current content postings omit positions and therefore require file verification. SQLite's numbers are not transferable to this format.

**Code fit:** `eval` already intersects in place and applies dense bitsets directly. `match_file` opens and reads fresh files. Each `verify` worker task allocates a local buffer and retains its capacity until that task finishes.

**Tradeoff:** Applying an extra posting list spends decode time to avoid potential file reads. Persistent reusable buffers save allocations but retain up to the chosen cap per reader between queries. A byte cache would additionally require freshness and invalidation rules; do not add one without evidence.

**Risks:** Preserve fresh-file reads, binary detection, smart case, regex behavior, matching-line text, candidate order, budgets, and limits. Changed candidate counts or read counts are observable. For limited/budgeted requests, changed timing can alter the returned prefix; verify unlimited responses separately. A cache validated only by second-resolution mtime and size can return stale content after same-size edits.

**Small experiment:** Collect phase data without changing behavior first. Then change one heuristic only. Compare unlimited file/line responses and separately inspect limited/budgeted results. Time warm and ordinary workloads separately, without clearing system caches. At most three rounds per invocation.

### 4. Tighten score bounds without skipping match counting

**Change:** First measure how often the existing bound activates. If scoring dominates, prototype per-name or per-block maxima for entry bonuses, replacing the global `kMaxEntryBonus` of 95 where a smaller proven maximum exists. Keep match verification and total counting.

**Evidence:** IBM's original [WAND publication](https://research.ibm.com/publications/efficient-query-evaluation-using-a-two-level-retrieval-process) describes partial candidate evaluation followed by full scoring. Its abstract allows effectiveness tradeoffs; that is not acceptable here. Use only mathematically safe bounds and do not transfer its reported evaluation reduction.

**Code fit:** `Engine::search` already has dynamic pruning. Exact totals prevent treating a losing score bound as permission to skip a matching block. Thus this principally saves scoring and TopK work, not all name scanning.

**Tradeoff:** Bonus metadata adds index bytes and build work. More bound loads can offset saved scoring. Mtime bonuses change with time; use a conservative bound of ten or derive bounds for the query's `now`. Overlay entries still need separate scoring.

**Risks:** Preserve strict score comparison: an equal score can win through folder/entry ties. Account for `.app` directory/link bonuses, priors, hidden penalties, clamped name scores, and every typo branch. Do not extend single-token pruning to ancestor scoring without proof. Do not repeat the rejected typo-score skip experiment under a different label.

**Small experiment:** Instrument pruned names and full scorings, then compare proposed bounds against actual scores across every existing matching fixture. Keep current traversal order and publication cadence. Compare exact totals and ordered scores at limits zero, one, and 50 before three-round timing.

### 5. Attribute concurrency and IPC costs before changing the pool

**Change:** Measure batch publication, caller work, completion wait, result merge, serialization, and client round trip separately. Distinguish hot typing from requests after workers have slept. Compare persistent connections with connection-per-request only if the client actually uses both paths.

**Evidence:** Apple's official [QoS guidance](https://developer.apple.com/library/archive/documentation/Performance/Conceptual/EnergyGuide-iOS/PrioritizeWorkWithQoS.html) says QoS affects scheduling, CPU/I/O throughput, and timer latency, with energy costs. It does not guarantee permanent performance-core placement or full clock. The guarantee implied by the comment in `pool.hpp` is not established by this source.

**Code fit:** Worker pools and multi-request socket connections already exist. Do not propose either as a new optimization. If allocation is significant, consider a reusable batch only after checking all pool callers and lifetimes. If idle-to-request delay dominates, evaluate the polling policy separately from throughput.

**Tradeoff:** Reusable batch state retains little memory but complicates ownership and synchronization. Longer polling spends CPU and energy between queries. Fewer active workers can reduce coordination yet lose parallelism; prior selective caller-only searches regressed on Chromium.

**Risks:** `Batch::fn` points to caller-owned state until every job completes. Reuse must preserve completion and publication ordering, and prove concurrent-run behavior. Pool changes can affect filename and content paths. Persistent connections must preserve framing, partial writes, disconnect handling, and query ordering. Do not alter response fields to simplify serialization.

**Small experiment:** Start with attribution only, using the existing eight-jobs baseline. Keep compiler, index, workers, QoS, and response sizes fixed. If one phase dominates, test one isolated change. Include slept-worker requests and ordinary hot replay, at most three rounds per invocation. Do not reopen rejected completion batching, counter separation, or batch claiming without new causal evidence.

## Acceptance and uncertainty

Prioritize exact response parity before median comparisons. Run existing search and disconnect checks for a future implementation. Record per-query classes, p90, daemon memory, and index bytes alongside medians. Instrumented runs are diagnostic, not timing evidence.

Use identical replayed workloads and isolated variants. Reverse variant order in a separate comparison. Every invocation must remain at three rounds or fewer. Chromium readiness failure is a blocker, not a latency result.

The ranking reflects implementation scope, semantic risk, and existing overlap, not predicted gains. Current phase costs, Apple Silicon gains, filename-gram selectivity, and whole-process memory impact remain UNKNOWN. Only this Markdown file was authored; no code, benchmark, installation, or application-control action was performed.
