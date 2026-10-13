# Vendored stb

This directory is a copy of three single-file libraries from stb, not a submodule or a fetched dependency.

| Field | Value |
| --- | --- |
| Source | `https://github.com/nothings/stb` |
| Version | commit `2c980bb59875b0d32144a71867fbdebb2f77cd20` (2026-08-01); `stb_image.h` v2.30, `stb_image_resize2.h` v2.18, `stb_image_write.h` v1.16 |
| Licence | MIT or public domain (Unlicense), the reader's choice: `LICENSE` |
| Copied | `stb_image.h`, `stb_image_resize2.h`, `stb_image_write.h`, `LICENSE` |
| Left out | Every other stb library, the tests, tools and documentation |
| Local changes | None |

The implementations are compiled once, in `src/stb_implementation.cpp`, into the target `sendspin_server_cpp_stb`, with their warnings off; the target sets the configuration macros (`STBI_ONLY_JPEG`, `STBI_ONLY_PNG`, `STBI_NO_STDIO`, `STBI_WRITE_NO_STDIO`) for its users too. That file also points stb_image's `STBI_MALLOC`, `STBI_REALLOC` and `STBI_FREE` at `src/stb_budget.h`, which bounds what a decode may allocate. The directory is a system include directory, so the headers raise no warnings where they are included.

To update, replace the three headers and `LICENSE` with those of a newer commit and update the table above. Do not edit files here; fix problems upstream and re-vendor.
