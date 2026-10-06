Third-party notices and product license

ISC-Lucide.txt covers the firmware's Lucide icon masks, derived from the
official Lucide 0.468.0 SVG sources (revision and source inventory in
tools/icons/sources/PROVENANCE.json). Original strokes are rasterized and
colored by the firmware; the Lucide copyright and permission notice are
distributed with both firmware and kit.

Kubik's own firmware, plugin, sounds, graphics and documentation are licensed
under the Apache License, Version 2.0. See LICENSE and NOTICE at the top of
the kit (copyright 2026 Pavel Frankov). This directory holds the notices of
third-party components that ship inside the firmware or that the plugin
installs. Every file below was copied from the component source on disk
(ESP-IDF 5.5.1 at ~/esp/esp-idf-v5.5.1, firmware/managed_components, the
plugin's node_modules) without changes, except the two notices written here
(BSD-3-Clause-TLSF.txt, Apache-2.0-OR-GPL-2.0-mbedTLS-notice.txt).

Apache-2.0.txt is the license text of ESP-IDF itself and of every Espressif
and Waveshare component below (all copies on disk are byte-identical):

  ESP-IDF 5.5.1 (drivers, FreeRTOS port, esp_http_server, nvs_flash, ...)
      Copyright Espressif Systems (Shanghai) CO LTD, Apache-2.0
  Espressif Wi-Fi, PHY and coexistence libraries (esp_wifi/lib, esp_phy/lib,
      esp_coex/lib: binary, linked into the image): Apache-2.0 per the LICENSE
      file next to them
  espressif/esp_codec_dev 1.4.0            Copyright 2023 Espressif, Apache-2.0
  espressif/esp_websocket_client 1.8.0     Copyright 2015-2026 Espressif, Apache-2.0
  espressif/esp_lcd_touch 1.2.1            Copyright 2015-2025 Espressif, Apache-2.0
  espressif/qrcode 0.2.0                   Copyright 2015-2021 Espressif, Apache-2.0
  waveshare/esp_lcd_touch_cst9217 1.0.4    Copyright 2015-2024 Espressif and
                                           2025 Waveshare, Apache-2.0

Other components:

  Apache-2.0-OR-GPL-2.0-mbedTLS-notice.txt  Mbed TLS 3.6.4 (used under Apache-2.0)
  BSD-lwIP*.txt                              lwIP (Swedish Institute of Computer Science)
                                             and files by CITEL Technologies, Inico
                                             Technologies, Leon Woestenberg (BSD)
  BSD-MINIX-tcp-isn.txt                      TCP initial sequence numbers (MINIX 3)
  BSD-wpa-supplicant.txt                     wpa_supplicant and hostapd code (Jouni
                                             Malinen and others), used under BSD
  BSD-3-Clause-TLSF.txt                      TLSF heap allocator (Matthew Conte)
  MIT-FreeRTOS.md                            FreeRTOS kernel (Amazon.com, Inc.)
  MIT-cJSON.txt                              cJSON (Dave Gamble and contributors)
  MIT-Nayuki-qrcodegen.txt                   QR Code generator library (Project Nayuki)
  http-parser.txt                            http-parser (NGINX code, Igor Sysoev; Joyent, Inc.)
  Newlib-COPYING-01..04.txt                  newlib C library of the toolchain
  GCC-COPYING3-01..02.txt, GCC-COPYING.RUNTIME.txt
                                             GCC runtime library (GPL-3.0 with the
                                             GCC Runtime Library Exception)
  OFL-PT-Sans.txt                            PT Sans and PT Sans Caption fonts
                                             (ParaType), SIL Open Font License 1.1,
                                             embedded as bitmap glyphs
  MIT-ws.txt                                 the `ws` npm package 8.22.0, the plugin's
                                             only runtime dependency; the plugin archive
                                             does not contain it, OpenClaw installs it
                                             from npm together with the plugin

The Newlib and GCC texts are split into numbered parts only to keep repository
files short; read each numbered series in ascending order as one document.

Not shipped, so not listed: zl38063 codec files of esp_codec_dev (not
compiled), OpenClaw itself (installed by the customer), ESP-IDF test and
example code.

TESS wake runtime: esp-tflite-micro 1.4.1, ESP-NN 1.4.1 and
esp-micro-speech-features 1.2.3 use Apache-2.0. ESP-SR 2.5.5 VAD uses
ESPRESSIF-MIT-esp-sr.txt (restricted to Espressif products). Third-party
gemmlowp uses Apache-2.0; FlatBuffers Apache-2.0; kissfft uses its BSD notice.
Tessa-model-notice.txt records the unresolved wake-model distribution rights.

BSD-SpeexDSP.txt: Xiph SpeexDSP 1.2.1 MDF acoustic echo cancellation, host WASM.

Native Muse: facebookincubator/muse-gadget-sdk, pinned revision
74a5e2d7fc895f109f83a9a1dbed705dbcd8b1ff. Copyright Meta Platforms, Inc. and
affiliates; Apache-2.0. Vendored Noise core, community pairing, C6 chat link
and software pairing test harness retain their source notices and LICENSE.
Source license does not grant rights to distribute a personal Muse SDK token.
