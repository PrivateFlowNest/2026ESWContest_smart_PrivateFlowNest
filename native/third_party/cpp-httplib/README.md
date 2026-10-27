# cpp-httplib 0.54.1 (vendored, unmodified)

HTTP/1.1 server for `native/services/homemind-mobile-api`.

- Upstream: https://github.com/yhirose/cpp-httplib, tag `v0.54.1` (`CPPHTTPLIB_VERSION "0.54.1"`)
- License: MIT (`LICENSE`, Copyright (c) 2017 yhirose)
- Form: upstream `httplib.h` split by upstream's own `split.py` into `httplib.h` + `httplib.cpp`, exactly as the
  pinned llama.cpp (`third_party/llama.cpp` `0f3a71be`, `vendor/cpp-httplib`, fetched by its `scripts/sync_vendor.py`)
  carries it. Copied from there byte for byte; no local changes.
- Built without TLS (`CPPHTTPLIB_OPENSSL_SUPPORT` not defined), no zlib/brotli. Compiled with `-w` as a separate
  static library (`hm_cpp_httplib`), so third-party warnings do not hit the tree's `-Werror`.

| file | bytes | SHA-256 |
|---|---|---|
| `httplib.h` | 172,015 | `0f497f1b662006fef9bfae72ad225aaf875255801aa60b59c7f202d790bd7671` |
| `httplib.cpp` | 627,480 | `bc08734ce4dcee6581cd081bf8c3d9b1bc29c4c9eb574f653cf4a8e5fdfbe875` |
| `LICENSE` | 1,097 | `bb546eae5924d8ee842e0e567e6d533c5824bdebccf605f99a5b3429034e6697` |

Updating: replace the three files with a newer upstream split (same `split.py`), update this table, rebuild and run
`homemind_mobile_api_tests` plus `native/tests/test_mobile_api_parity.py`.
