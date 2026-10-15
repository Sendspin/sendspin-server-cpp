# Third-party notices

sendspin-server-cpp is licensed under the Apache License 2.0 (`LICENSE`). It includes, or is built with, the third-party software listed below.

## Vendored

### micro-resampler

- Location: `third_party/micro-resampler/` (`VENDORED.md`)
- Source: this vendored copy; there is no public upstream repository
- Version: v0.0.6
- License: Apache-2.0 (`third_party/micro-resampler/LICENSE`)
- Copyright 2025-2026 Kevin Ahrendt

Its `NOTICE` file, reproduced as Apache-2.0 section 4(d) requires:

```text
microResampler
Copyright 2025-2026 Kevin Ahrendt

This product includes software developed by third parties.

================================================================================
Speex DSP Library - Kaiser Window Tables
================================================================================

src/detail/kaiser_tables.h contains Kaiser window table code derived from the
Speex DSP library (resample.c).

Copyright (C) 2007-2008 Jean-Marc Valin
Copyright (C) 2008 Thorvald Natvig

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

3. The name of the author may not be used to endorse or promote products
   derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO
EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
OF SUCH DAMAGE.
```

### stb

- Location: `third_party/stb/` (`stb_image.h`, `stb_image_resize2.h`, `stb_image_write.h`; `VENDORED.md`)
- Source: <https://github.com/nothings/stb>
- License: MIT or public domain (Unlicense), the reader's choice (`third_party/stb/LICENSE`)
- Copyright (c) 2017 Sean Barrett

```text
This software is available under 2 licenses -- choose whichever you prefer.
------------------------------------------------------------------------------
ALTERNATIVE A - MIT License
Copyright (c) 2017 Sean Barrett
Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:
The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
------------------------------------------------------------------------------
ALTERNATIVE B - Public Domain (www.unlicense.org)
This is free and unencumbered software released into the public domain.
Anyone is free to copy, modify, publish, use, compile, sell, or distribute this
software, either in source code form or as a compiled binary, for any purpose,
commercial or non-commercial, and by any means.
In jurisdictions that recognize copyright laws, the author or authors of this
software dedicate any and all copyright interest in the software to the public
domain. We make this dedication for the benefit of the public at large and to
the detriment of our heirs and successors. We intend this dedication to be an
overt act of relinquishment in perpetuity of all present and future rights to
this software under copyright law.
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

### sendspin-cpp

The sources listed in `src/ORIGIN.md` are copied or adapted from [sendspin-cpp](https://github.com/Sendspin/sendspin-cpp), Apache-2.0, Copyright 2026 Sendspin Contributors.

## Fetched at build time

CMake FetchContent downloads these at the pinned versions in `cmake/`; each is under its own license.

| Dependency | Source | License |
| --- | --- | --- |
| Boost (Beast, Asio and their dependencies) | <https://github.com/boostorg/boost> | BSL-1.0 |
| ArduinoJson | <https://github.com/bblanchon/ArduinoJson> | MIT |
| noise-c | <https://github.com/esphome-libs/noise-c> | MIT |
| micro-flac | <https://github.com/esphome-libs/micro-flac> | Apache-2.0 |
| micro-opus, with libopus | <https://github.com/esphome-libs/micro-opus> | Apache-2.0; libopus BSD-3-Clause |
| micro-ogg-demuxer (a submodule of micro-flac and micro-opus) | <https://github.com/esphome-libs/micro-ogg-demuxer> | Apache-2.0 |
| pffft | <https://github.com/marton78/pffft> | BSD-style (FFTPACK license) |
| GoogleTest (tests only) | <https://github.com/google/googletest> | BSD-3-Clause |
| sendspin-cpp (tests only) | <https://github.com/Sendspin/sendspin-cpp> | Apache-2.0 |
| IXWebSocket (tests only, fetched by sendspin-cpp) | <https://github.com/machinezone/IXWebSocket> | BSD-3-Clause |

## ESP-IDF build

The ESP-IDF component (`cmake/esp_idf.cmake`) builds with these in place of Boost, OpenSSL, the fetched noise-c and pffft, and without micro-opus; the vendored code, ArduinoJson and micro-flac (with micro-ogg-demuxer) are as above.

| Dependency | Source | License |
| --- | --- | --- |
| noise-c, the `esphome/noise-c` registry component | <https://github.com/esphome-libs/noise-c> | MIT |
| libsodium, the `esphome/libsodium` registry component that noise-c brings | <https://github.com/esphome-libs/libsodium> (a port of <https://github.com/jedisct1/libsodium>) | ISC; the port's packaging MIT |
| esp-dsp, the `espressif/esp-dsp` registry component | <https://github.com/espressif/esp-dsp> | Apache-2.0 |
| ESP-IDF, with its FreeRTOS, mbedTLS, lwIP and esp_http_server components | <https://github.com/espressif/esp-idf> | Apache-2.0; FreeRTOS MIT; mbedTLS Apache-2.0 OR GPL-2.0-or-later (<https://github.com/Mbed-TLS/mbedtls>); lwIP BSD-3-Clause |

## System libraries

OpenSSL 3 (Apache-2.0) on every host; avahi-client (LGPL-2.1-or-later) on Linux with discovery enabled; dns_sd, part of macOS, on macOS.
