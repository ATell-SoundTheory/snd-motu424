#!/usr/bin/env python3
"""Extract the MOTU PCI-424 family firmware images from MOTU's own macOS installer.

The firmware is copyrighted by MOTU and is NOT distributed with this project.
This script pulls it out of the installer you downloaded/own:

    MOTU Audio Installer 1.6 (73220).pkg  (or a compatible version)

It needs only the Python 3 standard library and works on Linux and macOS:
xar archive -> PCI_and_PCI_Express_Drivers.pkg/Payload (gzip'd cpio)
-> MOTUPCIAudio.kext/Contents/MacOS/MOTUPCIAudio (fat Mach-O)
-> x86_64 slice -> symbol table -> zlib-compressed blobs.

Output (default directory ./firmware/motu, install into /lib/firmware/motu/):
    pci424-altera.rbf      FPGA image for PCI-424 (137a:0003, TMS320C6205)
    pci424-dsp-prog.bin    C6205 program memory image (big-endian words)
    pci424-dsp-data.bin    C6205 data memory image (big-endian words)
    pci424b-altera.rbf     FPGA image for PCI-424X / PCIe-424 (137a:0004/0005)
    pci424b-dsp.bin        C6412 firmware image (big-endian words)
"""

import argparse
import gzip
import hashlib
import io
import os
import struct
import sys
import zlib

KEXT_SUFFIX = "MOTUPCIAudio.kext/Contents/MacOS/MOTUPCIAudio"

# Mangled symbol name -> output file name
BLOBS = {
    "__ZL17kPCI424AlteraData": "pci424-altera.rbf",
    "__ZL24kCompressedProgramMemory": "pci424-dsp-prog.bin",
    "__ZL21kCompressedDataMemory": "pci424-dsp-data.bin",
    "__ZL18kPCI424bAlteraData": "pci424b-altera.rbf",
    "__ZL20kPCI424bFirmwareData": "pci424b-dsp.bin",
}

# Inflated sizes observed in "MOTU Audio Installer 1.6 (73220)"; other
# versions may differ (a warning is printed, not an error).
KNOWN_SIZES = {
    "pci424-altera.rbf": 59216,
    "pci424-dsp-prog.bin": 37697,
    "pci424-dsp-data.bin": 13953,
    "pci424b-altera.rbf": 37177,
    "pci424b-dsp.bin": 28269,
}

CPU_TYPE_X86_64 = 0x01000007


def die(msg):
    sys.exit("extract_firmware: " + msg)


# ---------------------------------------------------------------- xar
def xar_files(data):
    """Yield (path, offset, length, encoding) for files in a xar archive."""
    import xml.etree.ElementTree as ET

    if data[:4] != b"xar!":
        die("not a xar archive (.pkg expected)")
    hsize, _ver, toc_clen, _toc_len, _ck = struct.unpack(">HHQQI", data[4:28])
    toc = zlib.decompress(data[hsize:hsize + toc_clen])
    heap = hsize + toc_clen
    root = ET.fromstring(toc).find("toc")

    def walk(elem, prefix):
        for f in elem.findall("file"):
            name = f.findtext("name")
            path = prefix + name
            d = f.find("data")
            if d is not None:
                enc = d.find("encoding").get("style")
                yield (path, heap + int(d.findtext("offset")),
                       int(d.findtext("length")), enc)
            yield from walk(f, path + "/")

    yield from walk(root, "")


def xar_read(data, offset, length, enc):
    raw = data[offset:offset + length]
    if "x-gzip" in enc or "zlib" in enc:
        return zlib.decompress(raw)
    if "x-bzip2" in enc:
        import bz2
        return bz2.decompress(raw)
    return raw


# ---------------------------------------------------------------- cpio (odc)
def cpio_find(stream, suffix):
    found = {}
    while True:
        hdr = stream.read(76)
        if len(hdr) < 76:
            break
        if hdr[:6] != b"070707":
            die("unsupported cpio format in payload")
        namesize = int(hdr[59:65], 8)
        filesize = int(hdr[65:76], 8)
        name = stream.read(namesize)[:-1].decode("utf-8", "replace")
        if name == "TRAILER!!!":
            break
        body = stream.read(filesize)
        if name.endswith(suffix):
            found[name] = body
    return found


# ---------------------------------------------------------------- Mach-O
def macho_x86_64(binary):
    magic = struct.unpack(">I", binary[:4])[0]
    if magic in (0xCAFEBABE,):
        n = struct.unpack(">I", binary[4:8])[0]
        for i in range(n):
            cpu, _sub, off, size, _al = struct.unpack(
                ">iiIII", binary[8 + 20 * i:28 + 20 * i])
            if cpu == CPU_TYPE_X86_64:
                return binary[off:off + size]
        die("no x86_64 slice in kext binary")
    if binary[:4] == b"\xcf\xfa\xed\xfe":
        return binary
    die("kext binary is not Mach-O")


def macho_symbols(m):
    """Return {name: file_offset} for symbols in a 64-bit Mach-O."""
    ncmds = struct.unpack("<I", m[16:20])[0]
    off = 32
    sections = []
    symtab = None
    for _ in range(ncmds):
        cmd, size = struct.unpack("<II", m[off:off + 8])
        if cmd == 0x19:  # LC_SEGMENT_64
            nsects = struct.unpack("<I", m[off + 64:off + 68])[0]
            for s in range(nsects):
                so = off + 72 + 80 * s
                addr, ssize, foff = struct.unpack(
                    "<QQI", m[so + 32:so + 52])
                sections.append((addr, ssize, foff))
        elif cmd == 0x2:  # LC_SYMTAB
            symtab = struct.unpack("<IIII", m[off + 8:off + 24])
        off += size
    if not symtab:
        die("kext has no symbol table")
    symoff, nsyms, stroff, _strsize = symtab
    out = {}
    for i in range(nsyms):
        e = symoff + 16 * i
        strx, ntype, _sect, _desc, value = struct.unpack(
            "<IBBHQ", m[e:e + 16])
        if ntype & 0xE0:  # stab
            continue
        end = m.index(b"\0", stroff + strx)
        name = m[stroff + strx:end].decode("ascii", "replace")
        for addr, ssize, foff in sections:
            if addr <= value < addr + ssize:
                out[name] = foff + (value - addr)
                break
    return out


def inflate_at(m, offset):
    d = zlib.decompressobj()
    out = d.decompress(m[offset:offset + 0x40000])
    if not d.eof:
        die("zlib stream at 0x%x is truncated" % offset)
    return out


# ---------------------------------------------------------------- main
def load_kext(path):
    data = open(path, "rb").read()
    if data[:4] == b"xar!":
        entries = list(xar_files(data))
        payloads = [e for e in entries
                    if e[0].endswith("PCI_and_PCI_Express_Drivers.pkg/Payload")]
        if not payloads:
            payloads = [e for e in entries if e[0].endswith("/Payload")]
        for path_, off, length, enc in payloads:
            payload = xar_read(data, off, length, enc)
            if payload[:2] == b"\x1f\x8b":
                payload = gzip.decompress(payload)
            found = cpio_find(io.BytesIO(payload), KEXT_SUFFIX)
            if found:
                # Prefer /Library/Extensions; all copies should be identical.
                name = sorted(found, key=lambda n: "/System/" in n)[0]
                print("found %s in %s" % (name, path_))
                return found[name]
        die("MOTUPCIAudio.kext not found in installer")
    return data  # already the kext binary (or a fat/thin Mach-O)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("installer",
                    help="MOTU Audio Installer .pkg, or the MOTUPCIAudio kext binary")
    ap.add_argument("-o", "--outdir", default="firmware/motu")
    args = ap.parse_args()

    m = macho_x86_64(load_kext(args.installer))
    syms = macho_symbols(m)
    os.makedirs(args.outdir, exist_ok=True)
    for sym, fname in BLOBS.items():
        if sym not in syms:
            die("symbol %s not found (unsupported driver version?)" % sym)
        blob = inflate_at(m, syms[sym])
        if len(blob) != KNOWN_SIZES[fname]:
            print("warning: %s is %d bytes (expected %d)"
                  % (fname, len(blob), KNOWN_SIZES[fname]))
        with open(os.path.join(args.outdir, fname), "wb") as f:
            f.write(blob)
        print("%-22s %6d bytes  sha256 %s" % (
            fname, len(blob), hashlib.sha256(blob).hexdigest()[:16]))
    print("\nInstall with:  sudo mkdir -p /lib/firmware/motu && "
          "sudo cp %s/* /lib/firmware/motu/" % args.outdir)


if __name__ == "__main__":
    main()
