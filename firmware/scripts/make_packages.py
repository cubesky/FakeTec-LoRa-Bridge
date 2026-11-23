# PlatformIO post-build: generate firmware.uf2 and firmware.zip from firmware.hex
#
#  firmware.uf2 : Adafruit nRF52 UF2 (family 0xADA52840) - copy to the bootloader drive
#                 (double-tap reset or 1200 bps touch), as used by the Meshtastic flasher
#  firmware.zip : legacy Nordic DFU package identical to
#                 `adafruit-nrfutil dfu genpkg --dev-type 0x0052 --sd-req 0x00B6`
#                 used by rnodeconf / RNode flasher, adafruit-nrfutil serial DFU,
#                 nRF Connect / Bluefruit OTA DFU
import json
import os
import re
import shutil
import struct
import zipfile

Import("env")  # noqa: F821  (provided by SCons)

UF2_FAMILY_NRF52840 = 0xADA52840
SD_FWID_S140_611 = 0x00B6
DEV_TYPE = 0x0052


def read_hex(path):
    mem = {}
    base = 0
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line.startswith(":"):
                continue
            raw = bytes.fromhex(line[1:])
            n, addr, typ = raw[0], (raw[1] << 8) | raw[2], raw[3]
            data = raw[4:4 + n]
            if typ == 0:
                for i, b in enumerate(data):
                    mem[base + addr + i] = b
            elif typ == 2:
                base = ((data[0] << 8) | data[1]) << 4
            elif typ == 4:
                base = ((data[0] << 8) | data[1]) << 16
            elif typ == 1:
                break
    lo, hi = min(mem), max(mem) + 1
    img = bytearray(b"\xff" * (hi - lo))
    for a, b in mem.items():
        img[a - lo] = b
    return lo, bytes(img)


def make_uf2(start, data):
    blocks = []
    nblocks = (len(data) + 255) // 256
    for i in range(nblocks):
        chunk = data[i * 256:(i + 1) * 256].ljust(256, b"\x00")
        hdr = struct.pack("<IIIIIIII", 0x0A324655, 0x9E5D5157, 0x00002000,
                          start + i * 256, 256, i, nblocks, UF2_FAMILY_NRF52840)
        blocks.append(hdr + chunk + b"\x00" * (476 - 256) + struct.pack("<I", 0x0AB16F30))
    return b"".join(blocks)


def crc16_ccitt(data, crc=0xFFFF):
    for b in data:
        crc = ((crc >> 8) & 0xFF) | ((crc << 8) & 0xFFFF)
        crc ^= b
        crc ^= (crc & 0xFF) >> 4
        crc ^= (crc << 12) & 0xFFFF
        crc ^= ((crc & 0xFF) << 5) & 0xFFFF
    return crc & 0xFFFF


def make_zip(path, app, stem):
    crc = crc16_ccitt(app)
    dat = struct.pack("<HHIH", DEV_TYPE, 0xFFFF, 0xFFFFFFFF, 1)
    dat += struct.pack("<H", SD_FWID_S140_611) + struct.pack("<H", crc)
    manifest = {
        "manifest": {
            "application": {
                "bin_file": stem + ".bin",
                "dat_file": stem + ".dat",
                "init_packet_data": {
                    "application_version": 0xFFFFFFFF,
                    "device_revision": 0xFFFF,
                    "device_type": DEV_TYPE,
                    "firmware_crc16": crc,
                    "softdevice_req": [SD_FWID_S140_611],
                },
            },
            "dfu_version": 0.5,
        }
    }
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr(stem + ".bin", app)
        z.writestr(stem + ".dat", dat)
        z.writestr("manifest.json", json.dumps(manifest, indent=4))


def build_packages(source, target, env):
    hexpath = str(target[0])
    base = os.path.splitext(hexpath)[0]
    start, img = read_hex(hexpath)
    with open(base + ".uf2", "wb") as f:
        f.write(make_uf2(start, img))
    make_zip(base + ".zip", img, os.path.basename(base))
    print("Packages: %s.uf2 / %s.zip (app @ 0x%X, %d bytes)" % (base, base, start, len(img)))

    # versioned copies for distribution: <project>/release/
    ver = "dev"
    for d in env.get("CPPDEFINES", []):
        if isinstance(d, (tuple, list)) and d[0] == "FW_VERSION":
            ver = re.sub(r'[^0-9A-Za-z._-]', "", str(d[1]))
    outdir = os.path.join(env.subst("$PROJECT_DIR"), "release")
    os.makedirs(outdir, exist_ok=True)
    stem = "firmware-faketec-lora-bridge-%s" % ver
    for ext in (".uf2", ".zip", ".hex"):
        shutil.copyfile(base + ext, os.path.join(outdir, stem + ext))
    print("Release: %s" % os.path.join(outdir, stem + ".{uf2,zip,hex}"))


env.AddPostAction("$BUILD_DIR/${PROGNAME}.hex", build_packages)  # noqa: F821
