# Third-Party Licenses

ThistleOS is BSD 3-Clause licensed and depends exclusively on Apache-2.0, MIT, BSD and zlib licensed components, on DejaVu fonts under the permissive Bitstream Vera license, and on Noto Emoji under the SIL Open Font License. No GPL or LGPL code is used.

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
**Used for:** UI rendering, widgets, animations, font rendering, touch input handling. The Arabic shaping every label and text field uses, `components/ui/src/arabic_shape.c`, is a port of LVGL's `lv_text_ap.c`.

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

## CrossPoint Reader

**License:** MIT
**Source:** https://github.com/crosspoint-reader/crosspoint-reader
**Used for:** The Reader's EPUB support. `apps/reader/reader/epub_zip.c`, `epub.c` and `html_entities.c` are C ports of CrossPoint's ZipFile, InflateReader, ContainerParser, ContentOpfParser, TocNcxParser, TocNavParser, the text rules of ChapterHtmlSlimParser, FsHelpers::normalisePath and htmlEntities.

```
Copyright (c) 2025 Dave Allie

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

---

## diy-esp32-epub-reader

**License:** MIT
**Source:** https://github.com/atomic14/diy-esp32-epub-reader
**Used for:** The HTML entity table in `apps/reader/reader/html_entities.c`, by way of CrossPoint Reader.

```
Copyright (c) 2021 Chris Greening

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

---

## minimp3

**License:** CC0-1.0 (public domain dedication)
**Source:** https://github.com/lieff/minimp3, commit `ea99364f61c14656440e8d77e9c233ccf3124633`
**Used for:** Decoding MP3 episodes in the Podcasts app. `minimp3.h` is vendored in `apps/podcasts/third_party/minimp3/` with its `LICENSE`; one change, a hook that puts the decoder's 16 KB scratch in PSRAM rather than on the stack, is described in the README beside it.

```
To the extent possible under law, the author(s) have dedicated all copyright
and related and neighboring rights to this software to the public domain
worldwide. This software is distributed without any warranty.
See <http://creativecommons.org/publicdomain/zero/1.0/>.
```

Full text: https://github.com/lieff/minimp3/blob/master/LICENSE

---

## libopus

**License:** BSD-3-Clause (Xiph.Org Foundation, Skype Limited, Octasic, Jean-Marc Valin, Timothy B. Terriberry, CSIRO, Gregory Maxwell, Mark Borgerding, Erik de Castro Lopo, Mozilla, Amazon)
**Source:** https://downloads.xiph.org/releases/opus/opus-1.5.2.tar.gz, SHA-256 `65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1`
**Used for:** Opus at 16 kHz mono for Tailnet chat's calls and voice messages. Vendored unchanged in `components/opus/opus-1.5.2/` with its `COPYING`, less the DNN, the float SILK, the ARM, x86 and MIPS code, the tests and the demos (`components/opus/opus.cmake`).

Full text: `components/opus/opus-1.5.2/COPYING`

---

## DejaVu fonts

**License:** Bitstream Vera Fonts license; DejaVu changes are in the public domain
**Version:** 2.37
**Source:** https://dejavu-fonts.github.io/
**Used for:** `apps/reader/reader/reader_font_16.c` and `reader_font_16_mono.c`, the bitmap fonts the Reader sets books in on colour screens and on e-paper, made from DejaVu Sans by `apps/reader/tools/gen_reader_font.sh`, which the Podcasts app sets its text in too; `components/ui/src/fonts/arabic_font_{14,18,22}.c` and their `_mono` cuts, the Arabic every label and text field falls back to, made from DejaVu Sans by `components/ui/tools/gen_arabic_fonts.sh`; and `apps/podcasts/podcasts/pod_symbols_16.c` and `pod_symbols_16_mono.c`, its two stars, made from DejaVu Sans by `apps/podcasts/tools/gen_symbols_font.sh`. LVGL's `lv_font_dejavu_16_persian_hebrew` is made from the same font.

```
Copyright (c) 2003 by Bitstream, Inc. All Rights Reserved. Bitstream Vera is
a trademark of Bitstream, Inc. DejaVu changes are in public domain.

Permission is hereby granted, free of charge, to any person obtaining a copy
of the fonts accompanying this license ("Fonts") and associated
documentation files (the "Font Software"), to reproduce and distribute the
Font Software, including without limitation the rights to use, copy, merge,
publish, distribute, and/or sell copies of the Font Software, and to permit
persons to whom the Font Software is furnished to do so, subject to the
following conditions:

The above copyright and trademark notices and this permission notice shall
be included in all copies of one or more of the Font Software typefaces.

The Font Software may be modified, altered, or added to, and in particular
the designs of glyphs or characters in the Fonts may be modified and
additional glyphs or characters may be added to the Fonts, only if the fonts
are renamed to names not containing either the words "Bitstream" or the word
"Vera".

This License becomes null and void to the extent applicable to Fonts or Font
Software that has been modified and is distributed under the "Bitstream
Vera" names.

The Font Software may be sold as part of a larger software package but no
copy of one or more of the Font Software typefaces may be sold by itself.

THE FONT SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
OR IMPLIED, INCLUDING BUT NOT LIMITED TO ANY WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT OF COPYRIGHT, PATENT,
TRADEMARK, OR OTHER RIGHT. IN NO EVENT SHALL BITSTREAM OR THE GNOME
FOUNDATION BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, INCLUDING
ANY GENERAL, SPECIAL, INDIRECT, INCIDENTAL, OR CONSEQUENTIAL DAMAGES,
WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF
THE USE OR INABILITY TO USE THE FONT SOFTWARE OR FROM OTHER DEALINGS IN THE
FONT SOFTWARE.

Except as contained in this notice, the names of Gnome, the Gnome
Foundation, and Bitstream Inc., shall not be used in advertising or
otherwise to promote the sale, use or other dealings in this Font Software
without prior written authorization from the Gnome Foundation or Bitstream
Inc., respectively. For further information, contact: fonts at gnome dot
org.
```

---

## Noto Emoji

**License:** SIL Open Font License 1.1 (no Reserved Font Name)
**Source:** https://github.com/google/fonts/tree/2fe1e8c6b137fb13041a7cfca6fdcd8a910ea4dc/ofl/notoemoji (`NotoEmoji[wght].ttf`, SHA-256 `de6c18832938afc99caf132b39d6a30a19bac7f2e812e28db2535b4608d27551`)
**Used for:** `apps/tailchat/tailchat/tailchat_emoji_{14,20}.c` and their `_mono` cuts, the emoji the Chat app draws in messages and reactions: about a hundred and ten glyphs cut from the font and turned into bitmaps by `apps/tailchat/tools/gen_emoji_font.sh` (lv_font_conv), a Modified Version under the licence below. They are compiled into the firmware with the app and never sold alone.

```
Copyright 2013 Google LLC

This Font Software is licensed under the SIL Open Font License, Version 1.1.
This license is copied below, and is also available with a FAQ at:
https://scripts.sil.org/OFL


-----------------------------------------------------------
SIL OPEN FONT LICENSE Version 1.1 - 26 February 2007
-----------------------------------------------------------

PREAMBLE
The goals of the Open Font License (OFL) are to stimulate worldwide
development of collaborative font projects, to support the font creation
efforts of academic and linguistic communities, and to provide a free and
open framework in which fonts may be shared and improved in partnership
with others.

The OFL allows the licensed fonts to be used, studied, modified and
redistributed freely as long as they are not sold by themselves. The
fonts, including any derivative works, can be bundled, embedded, 
redistributed and/or sold with any software provided that any reserved
names are not used by derivative works. The fonts and derivatives,
however, cannot be released under any other type of license. The
requirement for fonts to remain under this license does not apply
to any document created using the fonts or their derivatives.

DEFINITIONS
"Font Software" refers to the set of files released by the Copyright
Holder(s) under this license and clearly marked as such. This may
include source files, build scripts and documentation.

"Reserved Font Name" refers to any names specified as such after the
copyright statement(s).

"Original Version" refers to the collection of Font Software components as
distributed by the Copyright Holder(s).

"Modified Version" refers to any derivative made by adding to, deleting,
or substituting -- in part or in whole -- any of the components of the
Original Version, by changing formats or by porting the Font Software to a
new environment.

"Author" refers to any designer, engineer, programmer, technical
writer or other person who contributed to the Font Software.

PERMISSION & CONDITIONS
Permission is hereby granted, free of charge, to any person obtaining
a copy of the Font Software, to use, study, copy, merge, embed, modify,
redistribute, and sell modified and unmodified copies of the Font
Software, subject to the following conditions:

1) Neither the Font Software nor any of its individual components,
in Original or Modified Versions, may be sold by itself.

2) Original or Modified Versions of the Font Software may be bundled,
redistributed and/or sold with any software, provided that each copy
contains the above copyright notice and this license. These can be
included either as stand-alone text files, human-readable headers or
in the appropriate machine-readable metadata fields within text or
binary files as long as those fields can be easily viewed by the user.

3) No Modified Version of the Font Software may use the Reserved Font
Name(s) unless explicit written permission is granted by the corresponding
Copyright Holder. This restriction only applies to the primary font name as
presented to the users.

4) The name(s) of the Copyright Holder(s) or the Author(s) of the Font
Software shall not be used to promote, endorse or advertise any
Modified Version, except to acknowledge the contribution(s) of the
Copyright Holder(s) and the Author(s) or with their explicit written
permission.

5) The Font Software, modified or unmodified, in part or in whole,
must be distributed entirely under this license, and must not be
distributed under any other license. The requirement for fonts to
remain under this license does not apply to any document created
using the Font Software.

TERMINATION
This license becomes null and void if any of the above conditions are
not met.

DISCLAIMER
THE FONT SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO ANY WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT
OF COPYRIGHT, PATENT, TRADEMARK, OR OTHER RIGHT. IN NO EVENT SHALL THE
COPYRIGHT HOLDER BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
INCLUDING ANY GENERAL, SPECIAL, INDIRECT, INCIDENTAL, OR CONSEQUENTIAL
DAMAGES, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF THE USE OR INABILITY TO USE THE FONT SOFTWARE OR FROM
OTHER DEALINGS IN THE FONT SOFTWARE.
```

---

## MicroLink

**License:** MIT
**Source:** https://github.com/CamM2325/microlink, as forked at https://github.com/MedAouadhi/microlink (branch `thistle`)
**Used for:** The Tailscale client: the control plane, DERP, DISCO and STUN, behind `components/kernel/src/tailnet_platform.c`. S3 boards only.

```
MIT License

Copyright (c) 2025-2026 Cameron Malone
```

Full text: https://github.com/MedAouadhi/microlink/blob/thistle/LICENSE

It carries an X25519 implementation under the MIT License, Copyright (c) 2015-2016 Cryptography Research, Inc. (`x25519-license.txt` in its repository).

---

## wireguard-lwip

**License:** BSD-3-Clause
**Source:** https://github.com/smartalock/wireguard-lwip, as carried in MicroLink (`components/microlink/components/wireguard_lwip`)
**Used for:** WireGuard over lwIP: the tunnel netif the tailnet's traffic goes through.

```
Copyright (c) 2021 Daniel Hope (www.floorsense.nz)
All rights reserved.
```

Full text: `LICENSE` in that directory of MicroLink.

---

## cJSON

**License:** MIT
**Source:** https://github.com/DaveGamble/cJSON, as the `espressif/cjson` component
**Used for:** MicroLink's control-plane messages.

```
Copyright (c) 2009-2017 Dave Gamble and cJSON contributors
```

---

## md4c

**License:** MIT
**Source:** https://github.com/mity/md4c, release `release-0.5.2`, unmodified
**Used for:** Parsing Hermes's Markdown replies in the Hermes app, as hermes_esp32's firmware does. `md4c.c` and `md4c.h` are vendored in `apps/hermes/third_party/md4c/` with their `LICENSE.md`.

```
Copyright © 2016-2024 Martin Mitáš

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.
```

Full text: `apps/hermes/third_party/md4c/LICENSE.md`

---

## The Tailnet chat companion app (`companion/`)

The companion app is a separate program, built with Dart and Flutter for
Linux and Android; nothing of it is in the firmware. Its own code is BSD
3-Clause. Most of its pub.dev dependencies are BSD-3-Clause (Flutter,
`ffi`, `crypto`, `path_provider`, `flutter_local_notifications`, and the
transitive ones in `companion/app/pubspec.lock` and
`companion/core/pubspec.lock`). These are not:

| Package | Version | License | Used for |
|---------|---------|---------|----------|
| `dbus` (https://pub.dev/packages/dbus) | 0.8.0 | **MPL-2.0** | Linux: the identity key kept in the Secret Service (GNOME Keyring, KWallet), over D-Bus (`companion/app`) |
| `ed25519_edwards` (https://pub.dev/packages/ed25519_edwards) | 0.3.2 | Apache-2.0 | Ed25519 signatures for the handshake (`companion/core`) |
| `sqlite3` (https://pub.dev/packages/sqlite3) | 3.8.0 | MIT | The message store (`companion/core`); the SQLite library it loads is public domain |
| `flutter_webrtc` (https://pub.dev/packages/flutter_webrtc) | 1.6.3 | MIT | WebRTC calls between companion apps (`companion/app`, ADR-0016); with it `dart_webrtc` 1.8.2, `webrtc_interface` 1.5.1, `logger` 2.8.0 and `synchronized` 3.5.0, all MIT |

**libwebrtc, inside the Linux app.** flutter_webrtc's Linux build links a
prebuilt `libwebrtc.so` (24 MB), release `libwebrtc.m150.7871.07` of
github.com/webrtc-sdk/libwebrtc, fetched by the build and checked against
the SHA-256 in `companion/app/linux/libwebrtc.cmake`, and shipped in the
bundle's `lib/`. Its wrapper is MIT (the zip's `LICENSE`); what it is built
from is Chromium's WebRTC (BSD-3-Clause) and the third-party code Chromium
builds into it, among them:

| Part | License | In the binary for |
|------|---------|-------------------|
| WebRTC, libyuv, libvpx, libsrtp, usrsctp, Opus | BSD-3-Clause | The call itself; VP8, the codec the app prefers, and Opus |
| BoringSSL | OpenSSL / ISC | DTLS-SRTP, which runs inside WireGuard and is not relied on |
| Abseil | Apache-2.0 | WebRTC's base library |
| dav1d | BSD-2-Clause | AV1 decoding, never negotiated by the app |
| OpenH264 | BSD-2-Clause | H.264, which the app never offers (patent-encumbered code is in the binary all the same) |
| FFmpeg (Chromium's build) | **LGPL-2.1-or-later** | H.264 decoding, never negotiated by the app |

FFmpeg is LGPL-2.1: the app links it dynamically, inside `libwebrtc.so`,
which a user may replace with another build of the same release; its
source is Chromium's `third_party/ffmpeg` at the WebRTC revision the
release names. No GPL. The Android build's WebRTC comes from Maven
(`io.github.webrtc-sdk:android` 150.7871.04, the same Chromium WebRTC and
its BSD-3-Clause third-party code) and has no software H.264; with it
comes flutter_webrtc's audio switch, `com.github.davidliu:audioswitch`
(a fork of Twilio's AudioSwitch, Apache-2.0), from JitPack. Both, like
every artifact the Android build fetches, are checked against the
SHA-256 in `companion/app/android/gradle/verification-metadata.xml`.

The app bundles three fonts, each with its licence beside it in
`companion/app/assets/fonts/`, from github.com/google/fonts (`ofl/`):

| Font | License | Used for |
|------|---------|----------|
| Fraunces (variable) | SIL OFL 1.1 | Names and titles |
| Instrument Sans (variable) | SIL OFL 1.1 | Text |
| JetBrains Mono (variable) | SIL OFL 1.1 | Fingerprints, addresses, times |

MPL-2.0 is a file-level copyleft: the `dbus` package's own files stay
under MPL-2.0, and changes to them would have to be published under it;
the app that uses them, unchanged, may stay BSD 3-Clause. The Android
build also pulls AndroidX and Kotlin libraries through Gradle, under
Apache-2.0. Re-check this table when a companion `pubspec.yaml` changes.

---

## License Compatibility Summary

All dependencies are compatible with the ThistleOS BSD 3-Clause license:

| License | Compatible with BSD 3-Clause | Components |
|---------|------------------------------|------------|
| Apache-2.0 | Yes | ESP-IDF, esp_modem, esp_codec_dev, elf_loader, NimBLE, mbedTLS, ed25519_edwards (companion app) |
| MIT | Yes | LVGL, FreeRTOS, RadioLib, libcurl, Unity, esp-rs crates, Expat, CrossPoint Reader, diy-esp32-epub-reader, MicroLink, cJSON, md4c, sqlite3, flutter_webrtc and the libwebrtc wrapper (companion app) |
| BSD-3-Clause | Yes | Bosch BHI260AP firmware and BHY2 SensorAPI, wireguard-lwip |
| BSD-2-Clause | Yes | Monocypher |
| zlib | Yes | SDL2, uzlib |
| CC0-1.0 | Yes | minimp3 |
| Bitstream Vera | Yes | DejaVu fonts (the Reader's font) |
| MIT OR Apache-2.0 | Yes | argon2, serde_json, esp-idf-hal, esp-idf-svc, esp-idf-sys |
| MPL-2.0 (file-level copyleft) | Yes, used unmodified | dbus (companion app only) |
| LGPL-2.1-or-later | Yes, dynamically linked and unmodified | Chromium's FFmpeg inside libwebrtc.so (companion app on Linux only) |
| SIL OFL 1.1 | Yes, not sold alone | Fraunces, Instrument Sans, JetBrains Mono (companion app, unmodified); Noto Emoji (the Chat app's emoji, cut to bitmaps) |

No GPL, LGPL or AGPL code is in the firmware. The copyleft dependencies elsewhere are the companion app's: `dbus`, MPL-2.0, and on Linux the FFmpeg inside libwebrtc, LGPL-2.1, above.
