# fplussearch versus fsearch

Final native comparison, 2026-10-10. Each engine ran in an isolated process for three rounds in forward and reverse order. fplussearch was executable SHA-256 `7e62e1af1b8d99098174b2a799ea4696110850cfc1bd373449b6c0a8c0273c23`.

The fsearch library was frozen from clean ref `eccb97d5649730508f0d82f6047b7355cb424a1c` (`src/content.rs` SHA-256 `88966e54ef73ea6d2b092254bcd3238fbd8205024fe6ff96fd7f9f7b449e9237`) and compiled with `RUSTFLAGS=-C target-cpu=native`.

| Dataset | Order | Metric | fplussearch | fsearch |
| --- | --- | --- | ---: | ---: |
| Linux | forward | filename p50 (ms) | 0.0469 | 0.0480 |
| Linux | reverse | filename p50 (ms) | 0.0427 | 0.0443 |
| Chromium | forward | filename p50 (ms) | 0.0756 | 0.0815 |
| Chromium | reverse | filename p50 (ms) | 0.0736 | 0.0866 |
| Linux | forward | grep p50 (ms) | 0.530 | 0.945 |
| Linux | reverse | grep p50 (ms) | 0.532 | 0.995 |
| Chromium | forward | grep p50 (ms) | 1.509 | 1.527 |
| Chromium | reverse | grep p50 (ms) | 1.542 | 1.667 |

Both datasets report `baseline_parity: true`; filename and full content result digests match the clean baseline in both orders. The reverse harness also validates limited content-result parity and records per-query `verification_counts` with `read` and `candidates` fields from fplussearch's server response. Exact top-result accuracy is 100%; typo accuracy is 98.75% on Linux and 99.67% on Chromium.

Ready fplussearch footprint was about 30 MiB on Linux and 62 MiB on Chromium in the forward run. These timings use the user's selected index-refresh behavior, including the one-second `in:` scope freshness window.

Raw measurements are in `benchmarks/results/beat-fsearch-final-forward.json` and `benchmarks/results/beat-fsearch-final-reverse.json`; the latter contains the verification counters and limited parity digest.
