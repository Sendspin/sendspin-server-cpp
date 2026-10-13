# Vendored micro-resampler

This directory is a copy of micro-resampler, not a submodule or a fetched dependency.

| Field | Value |
| --- | --- |
| Library | micro-resampler |
| Version | v0.0.6 |
| License | Apache-2.0: `LICENSE`, with the third-party notices in `NOTICE` |
| Copied | `CMakeLists.txt`, `LICENSE`, `NOTICE`, `include/`, `src/` |
| Left out | Everything else: its README, tests, examples, ESP-IDF packaging, docs, scripts, tools, CI and editor configuration |
| Local changes | None |

Consume it with `add_subdirectory(third_party/micro-resampler)`; as a subproject it builds only the library, with the consumer's compiler flags (`MICRO_RESAMPLER_BUILD_TESTS` and `MICRO_RESAMPLER_NATIVE_TUNING` default off).

Do not edit files here; to update, re-vendor the same paths from a newer release and update the table above.
