SPDX-License-Identifier: GPL-2.0-only OR MIT

The public-uapi.rs declaration snapshot includes the optional memory_limit and
client_memory_limit tail fields from the public MIT C UAPI at kernel commit
a1b269f79e35844660892b9779adb2f68abc38b7 (include/uapi/drm/asahi_drm.h).
The preceding layout remains the legacy 600-byte reply prefix; the full struct
is 616 bytes. test-uapi.py compiles the C and Rust declarations and compares
their sizes, offsets and constants, including the paired Mesa header.

The independent C-generated encoder and VM byte fixtures were not regenerated
or changed. Their original provenance and hashes remain in manifest.json.
