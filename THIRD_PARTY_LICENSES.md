# Third-Party Licenses

ThistleOS is BSD 3-Clause licensed and depends exclusively on Apache-2.0, MIT, BSD and zlib licensed components. No GPL or LGPL code is used.

---

## ESP-IDF

**License:** Apache License 2.0
**Source:** https://github.com/espressif/esp-idf
**Used for:** Build system, FreeRTOS integration, WiFi stack, BLE (NimBLE), mbedTLS, SPIFFS, SD/MMC, esp_lcd, PPP (LwIP), partition management, OTA, nvs_flash.

```
Copyright 2016-2024 Espressif Systems (Shanghai) CO LTD

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0
```

Full text: https://github.com/espressif/esp-idf/blob/master/LICENSE

---

## FreeRTOS

**License:** MIT
**Source:** https://www.freertos.org/
**Used for:** Real-time OS kernel, task scheduling, queues, semaphores. Included via ESP-IDF.

```
Copyright (C) 2021 Amazon.com, Inc. or its affiliates.  All Rights Reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
the Software, and to permit persons to whom the Software is furnished to do so,
subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

Full text: https://github.com/FreeRTOS/FreeRTOS-Kernel/blob/main/LICENSE.md

---

## LVGL

**License:** MIT
**Version:** 9.2
**Source:** https://github.com/lvgl/lvgl
**Used for:** UI rendering, widgets, animations, font rendering, touch input handling.

```
MIT License

Copyright (c) 2021 LVGL Kft.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

Full text: https://github.com/lvgl/lvgl/blob/master/LICENCE.txt

---

## esp_lvgl_port

**License:** Apache License 2.0
**Source:** https://github.com/espressif/esp-bsp/tree/master/components/esp_lvgl_port
**Used for:** Integration layer between LVGL 9 and ESP-IDF display/input drivers.

```
Copyright 2023 Espressif Systems (Shanghai) CO LTD

Licensed under the Apache License, Version 2.0.
```

Full text: https://github.com/espressif/esp-bsp/blob/master/LICENSE

---

## RadioLib

**License:** MIT
**Source:** https://github.com/jgromes/RadioLib
**Used for:** SX1262 LoRa radio driver (`drv_radio_sx1262`).

```
MIT License

Copyright (c) 2018 Jan Gromes

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

Full text: https://github.com/jgromes/RadioLib/blob/master/LICENSE

---

## esp_modem

**License:** Apache License 2.0
**Source:** https://github.com/espressif/esp-protocols/tree/master/components/esp_modem
**Used for:** A7682E 4G modem PPP networking (`drv_modem_a7682e`).

```
Copyright 2021-2023 Espressif Systems (Shanghai) CO LTD

Licensed under the Apache License, Version 2.0.
```

Full text: https://github.com/espressif/esp-protocols/blob/master/LICENSE

---

## esp_codec_dev

**License:** Apache License 2.0
**Source:** https://github.com/espressif/esp-adf/tree/master/components/esp_codec_dev
**Used for:** ES8311 audio codec register initialisation and playback (`drv_audio_es8311`).

```
Copyright 2023 Espressif Systems (Shanghai) CO LTD

Licensed under the Apache License, Version 2.0.
```

Full text: https://github.com/espressif/esp-adf/blob/master/components/esp_codec_dev/LICENSE

---

## Bosch BHI260AP firmware

**License:** BSD 3-Clause (Bosch Sensortec BHY2 SensorAPI), redistributed under MIT by SensorLib
**Source:** https://github.com/boschsensortec/BHY2_SensorAPI, via the copy LilyGo ships in https://github.com/Xinyuan-LilyGO/T-Deck-MAX (`lib/SensorLib/src/bosch/firmware/BHI260AP.fw.h`)
**Used for:** The firmware image the BHI260AP IMU driver (`kernel_rs/src/drv_imu_bhi260ap.rs`) uploads into the sensor hub at every init. Stored as `components/kernel_rs/firmware/bhi260ap.fw`, the header's byte array converted to binary unchanged (103 676 bytes, SHA-256 `318def511fd8eb762bf1e61cf02cfd6ecac70f8627d3bacfa7d24a1641f9e915`). The host-interface protocol the driver speaks was ported from the same SensorAPI sources.

```
Copyright (c) 2023 Bosch Sensortec GmbH. All rights reserved.

BSD-3-Clause

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its
   contributors may be used to endorse or promote products derived from
   this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.
```

The firmware header carries no notice of its own; the notice above is the one on the SensorAPI sources it ships alongside.

---

## espressif__elf_loader

**License:** Apache License 2.0
**Source:** https://github.com/espressif/esp-idf-elf-loader
**Used for:** Dynamic ELF loading of apps and drivers from SD card at runtime.

```
Copyright 2023 Espressif Systems (Shanghai) CO LTD

Licensed under the Apache License, Version 2.0.
```

Full text: https://github.com/espressif/esp-idf-elf-loader/blob/main/LICENSE

---

## NimBLE (Apache Mynewt NimBLE)

**License:** Apache License 2.0
**Source:** https://github.com/apache/mynewt-nimble
**Used for:** BLE 5.0 stack. Included via ESP-IDF as `bt/host/nimble`.

```
Copyright 2015-2021 The Apache Software Foundation

Licensed under the Apache License, Version 2.0.
```

Full text: https://github.com/apache/mynewt-nimble/blob/master/LICENSE

---

## Monocypher

**License:** BSD-2-Clause OR CC0-1.0 (public domain)
**Version:** 4.x
**Source:** https://monocypher.org/ / https://github.com/LoupVaillant/Monocypher
**Used for:** Ed25519 signature verification for apps, drivers, and firmware.

```
Copyright (c) 2017-2023, Loup Vaillant
Copyright (c) 2017-2019, Michael Savage
Copyright (c) 2017-2023, Fabio Scotoni

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

Full text: https://github.com/LoupVaillant/Monocypher/blob/master/LICENCE.md

---

## mbedTLS

**License:** Apache License 2.0
**Source:** https://github.com/Mbed-TLS/mbedtls
**Used for:** TLS (HTTPS app store), AES-256-CBC (Vault), SHA-256 OTA verification, and legacy PBKDF2 vault migration. Included via ESP-IDF.

```
Copyright The Mbed TLS Contributors

Licensed under the Apache License, Version 2.0.
```

Full text: https://github.com/Mbed-TLS/mbedtls/blob/development/LICENSE

---

## SDL2 (Simple DirectMedia Layer)

**License:** zlib License
**Source:** https://github.com/libsdl-org/SDL
**Used for:** Simulator display rendering, keyboard/mouse input, window management. Desktop builds only — not included in firmware.

```
Copyright (C) 1997-2024 Sam Lantinga <slouken@libsdl.org>

This software is provided 'as-is', without any express or implied
warranty.  In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software. If you use this software
   in a product, an acknowledgment in the product documentation would be
   appreciated but is not required.
2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.
3. This notice may not be removed or altered from any source distribution.
```

Full text: https://github.com/libsdl-org/SDL/blob/main/LICENSE.txt

---

## libcurl

**License:** MIT/X derivative (curl license)
**Source:** https://curl.se/
**Used for:** HTTP/HTTPS client in the desktop simulator (app store, OTA). Not included in firmware.

```
COPYRIGHT AND PERMISSION NOTICE

Copyright (c) 1996 - 2024, Daniel Stenberg, <daniel@haxx.se>, and many
contributors, see the THANKS file.

All rights reserved.

Permission to use, copy, modify, and distribute this software for any purpose
with or without fee is hereby granted, provided that the above copyright
notice and this permission notice appear in all copies.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT OF THIRD PARTY RIGHTS. IN
NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE
OR OTHER DEALINGS IN THE SOFTWARE.
```

Full text: https://curl.se/docs/copyright.html

---

## Unity (Test Framework)

**License:** MIT
**Source:** https://github.com/ThrowTheSwitch/Unity
**Used for:** Unit test framework (`components/test_thistle/`). Not included in production firmware.

```
Copyright (c) 2007-2024 Mike Karlesky, Mark VanderVoord, Greg Williams

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

Full text: https://github.com/ThrowTheSwitch/Unity/blob/master/LICENSE.txt

---

## esp-idf-hal (Rust)

**License:** MIT OR Apache-2.0
**Source:** https://github.com/esp-rs/esp-idf-hal
**Used for:** Rust HAL bindings for ESP32-S3 peripherals in the Recovery OS.

Full text: https://github.com/esp-rs/esp-idf-hal/blob/master/LICENSE-MIT
and: https://github.com/esp-rs/esp-idf-hal/blob/master/LICENSE-APACHE

---

## argon2 (RustCrypto)

**License:** MIT OR Apache-2.0
**Source:** https://github.com/RustCrypto/password-hashes/tree/master/argon2
**Used for:** Argon2id password key derivation for vault and messaging envelopes.

Full text: https://github.com/RustCrypto/password-hashes/blob/master/LICENSE-MIT
and: https://github.com/RustCrypto/password-hashes/blob/master/LICENSE-APACHE

---

## serde_json

**License:** MIT OR Apache-2.0
**Source:** https://github.com/serde-rs/json
**Used for:** Safe parsing and serialization of persistent system configuration.

Full text: https://github.com/serde-rs/json/blob/master/LICENSE-MIT
and: https://github.com/serde-rs/json/blob/master/LICENSE-APACHE

---

## esp-idf-svc (Rust)

**License:** MIT OR Apache-2.0
**Source:** https://github.com/esp-rs/esp-idf-svc
**Used for:** Rust high-level service wrappers (WiFi, HTTP server) in the Recovery OS.

Full text: https://github.com/esp-rs/esp-idf-svc/blob/master/LICENSE-MIT
and: https://github.com/esp-rs/esp-idf-svc/blob/master/LICENSE-APACHE

---

## esp-idf-sys (Rust)

**License:** MIT OR Apache-2.0
**Source:** https://github.com/esp-rs/esp-idf-sys
**Used for:** Low-level Rust bindings to ESP-IDF C APIs in the Recovery OS.

Full text: https://github.com/esp-rs/esp-idf-sys/blob/master/LICENSE-MIT

---

## Expat

**License:** MIT
**Version:** 2.7.3
**Source:** https://github.com/libexpat/libexpat
**Used for:** Parsing the XML of EPUB books in the Reader. Vendored unmodified in `apps/reader/third_party/expat/`, with its `COPYING`.

```
Copyright (c) 1998-2000 Thai Open Source Software Center Ltd and Clark Cooper
Copyright (c) 2001-2025 Expat maintainers

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

Full text: https://github.com/libexpat/libexpat/blob/master/expat/COPYING

---

## uzlib

**License:** zlib
**Source:** https://github.com/pfalcon/uzlib
**Used for:** Inflating the deflate-compressed files inside EPUB books. Vendored in `apps/reader/third_party/uzlib/` with its `LICENSE`; one signedness cast is added in `tinflate.c`, as in CrossPoint Reader's copy.

```
Copyright (c) 2003 by Joergen Ibsen / Jibz
All Rights Reserved
http://www.ibsensoftware.com/

Copyright (c) 2014-2018 by Paul Sokolovsky

This software is provided 'as-is', without any express
or implied warranty.  In no event will the authors be
held liable for any damages arising from the use of
this software.

Permission is granted to anyone to use this software
for any purpose, including commercial applications,
and to alter it and redistribute it freely, subject to
the following restrictions:

1. The origin of this software must not be
   misrepresented; you must not claim that you
   wrote the original software. If you use this
   software in a product, an acknowledgment in
   the product documentation would be appreciated
   but is not required.

2. Altered source versions must be plainly marked
   as such, and must not be misrepresented as
   being the original software.

3. This notice may not be removed or altered from
   any source distribution.
```

Full text: https://github.com/pfalcon/uzlib/blob/master/LICENSE

---

## License Compatibility Summary

All dependencies are compatible with the ThistleOS BSD 3-Clause license:

| License | Compatible with BSD 3-Clause | Components |
|---------|------------------------------|------------|
| Apache-2.0 | Yes | ESP-IDF, esp_lvgl_port, esp_modem, esp_codec_dev, elf_loader, NimBLE, mbedTLS |
| MIT | Yes | LVGL, FreeRTOS, RadioLib, libcurl, Unity, esp-rs crates, Expat |
| BSD-3-Clause | Yes | Bosch BHI260AP firmware and BHY2 SensorAPI |
| BSD-2-Clause | Yes | Monocypher |
| zlib | Yes | SDL2, uzlib |
| MIT OR Apache-2.0 | Yes | argon2, serde_json, esp-idf-hal, esp-idf-svc, esp-idf-sys |

No GPL, LGPL, AGPL, or other copyleft licenses are present in this project.
