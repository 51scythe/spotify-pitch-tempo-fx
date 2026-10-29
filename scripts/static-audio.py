"""Read-only, hash-bound focused Spotify.dll static evidence (pefile/Capstone).

Addresses accepted by --rva/--xref are RVAs. --offset translates raw offsets.
Xrefs scan direct rel32 calls/jumps and common RIP-relative memory instructions;
this is a bounded candidate search, not a complete control/data-flow analysis.
"""
import argparse
import bisect
import hashlib
import re
import struct
import uuid
from pathlib import Path

import capstone
import pefile

ROOT = Path(__file__).resolve().parents[1]
EXPECTED = "93ba5ee486e148114a1d6db388cce318b04d2a011376ff6301f5ee0dea432a12"
SEEDS = [0x1B01E78, 0x1B024A0, 0x1B025A0, 0x1B02708, 0x1AAD910]


def number(s):
    return int(s, 0)


def main():
    ap = argparse.ArgumentParser(__doc__)
    ap.add_argument("--binary", type=Path, default=ROOT / "originals/Spotify.dll")
    ap.add_argument("--rva", type=number, action="append", default=[])
    ap.add_argument("--table", type=number, action="append", default=[],
                    help="Read image-VA qwords at an RVA (bytes: --size, default 0x80)")
    ap.add_argument("--xref", type=number, action="append", default=[])
    ap.add_argument("--offset", type=number, action="append", default=[])
    ap.add_argument("--find-text", action="append", default=[])
    ap.add_argument("--guid", action="append", default=[], help="Find a GUID's little-endian bytes")
    ap.add_argument("--size", type=number, help="explicit disassembly byte length")
    ap.add_argument("--out", type=Path)
    args = ap.parse_args()
    data = args.binary.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    if digest != EXPECTED:
        raise SystemExit("Target SHA-256 differs: " + digest)
    pe = pefile.PE(data=data, fast_load=True)
    pe.parse_data_directories(directories=[
        pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXCEPTION"],
        pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_IMPORT"],
    ])
    base = pe.OPTIONAL_HEADER.ImageBase
    functions = sorted((e.struct.BeginAddress, e.struct.EndAddress,
                        e.struct.UnwindData) for e in pe.DIRECTORY_ENTRY_EXCEPTION)
    starts = [f[0] for f in functions]
    imports = {i.address: d.dll.decode() + "!" +
               (i.name.decode() if i.name else "ordinal_" + str(i.ordinal))
               for d in pe.DIRECTORY_ENTRY_IMPORT for i in d.imports}
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True
    lines = []
    boundaries = {}

    def emit(s=""):
        lines.append(s)

    def owner(rva):
        i = bisect.bisect_right(starts, rva) - 1
        return functions[i] if i >= 0 and rva < functions[i][1] else None

    def loc(rva):
        try:
            raw = pe.get_offset_from_rva(rva)
        except pefile.PEFormatError:
            raw = None
        f = owner(rva)
        return (f"RVA=0x{rva:x} VA=0x{base+rva:x} raw=" +
                (f"0x{raw:x}" if raw is not None else "unmapped") +
                (f" unwind=[0x{f[0]:x},0x{f[1]:x}) data=0x{f[2]:x}" if f else ""))

    def label(va):
        if va in imports:
            return imports[va]
        rva = va - base
        try:
            raw = pe.get_offset_from_rva(rva)
            text = data[raw:raw+220].split(b"\0")[0]
            if len(text) >= 4 and all(32 <= c < 127 or c in (9, 10, 13) for c in text):
                return repr(text.decode())
            return "data=" + data[raw:raw+16].hex()
        except (pefile.PEFormatError, ValueError):
            return ""

    def instruction(ins):
        notes = []
        for op in ins.operands:
            if op.type == capstone.x86.X86_OP_MEM and op.mem.base == capstone.x86.X86_REG_RIP:
                target = ins.address + ins.size + op.mem.disp
                notes.append(f"->0x{target:x} " + label(target))
            elif op.type == capstone.x86.X86_OP_IMM and ins.group(capstone.CS_GRP_CALL):
                notes.append(f"callee RVA=0x{op.imm-base:x}")
        return (f"{ins.address:016x}  {ins.bytes.hex():24s} {ins.mnemonic:9s} " +
                ins.op_str + (" ; " + " | ".join(notes) if notes else ""))

    emit(f"SHA256={digest} bytes={len(data)} imagebase=0x{base:x} machine=0x{pe.FILE_HEADER.Machine:x}")
    emit(f"pefile={pefile.__version__} capstone={capstone.__version__} STATIC ONLY")
    for s in pe.sections:
        emit(f"section {s.Name.rstrip(bytes([0])).decode()} RVA=0x{s.VirtualAddress:x} raw=0x{s.PointerToRawData:x} size=0x{s.SizeOfRawData:x}")

    offsets = args.offset[:]
    if not (args.rva or args.table or args.xref or args.offset or args.find_text or args.guid):
        offsets.extend(SEEDS)
    for term in args.find_text:
        for match in re.finditer(re.escape(term.encode()), data):
            offsets.append(data.rfind(b"\0", 0, match.start()) + 1)
    targets = set(args.xref)
    for raw in offsets:
        rva = pe.get_rva_from_offset(raw)
        targets.add(rva)
        emit("STRING " + loc(rva) + " " + label(base+rva))
    for guid in args.guid:
        matches = list(re.finditer(re.escape(uuid.UUID(guid).bytes_le), data))
        emit("GUID " + guid + " matches=" + str(len(matches)))
        for match in matches:
            rva = pe.get_rva_from_offset(match.start())
            targets.add(rva)
            emit("GUID_LOCATION " + loc(rva))
    if targets:
        # Common REX/legacy RIP-relative mov/lea/indirect-call and direct rel32.
        patterns = [re.compile(rb"(?=([\x40-\x4f]?[\x8d\x8b\x89\xff][\x05\x0d\x15\x1d\x25\x2d\x35\x3d].{4}))", re.S),
                    re.compile(rb"(?=([\xe8\xe9].{4}))", re.S)]
        seen = set()
        for section in pe.sections:
            if not section.Characteristics & 0x20000000:
                continue
            code = section.get_data()
            for pattern in patterns:
                for m in pattern.finditer(code):
                    candidate = m.group(1)
                    rva = section.VirtualAddress + m.start()
                    dest = rva + len(candidate) + struct.unpack("<i", candidate[-4:])[0]
                    if dest not in targets or (rva, dest) in seen:
                        continue
                    seen.add((rva, dest))
                    f = owner(rva)
                    if f:
                        if f[0] not in boundaries:
                            boundaries[f[0]] = {ins.address-base for ins in md.disasm(
                                pe.get_data(f[0], f[1]-f[0]), base+f[0])}
                        if rva not in boundaries[f[0]]:
                            continue
                    decoded = next(md.disasm(candidate, base+rva), None)
                    if decoded and decoded.size == len(candidate):
                        emit("XREF target=" + hex(dest) + " " + loc(rva))
                        emit(instruction(decoded))

    for rva in args.table:
        size = args.size if args.size is not None else 0x80
        if size < 0 or size % 8:
            raise SystemExit("Table size must be a nonnegative multiple of 8")
        table = pe.get_data(rva, size)
        if len(table) != size:
            raise SystemExit("Incomplete table read at " + hex(rva))
        emit("\nQWORD_TABLE " + loc(rva) + f" bytes=0x{size:x}")
        for offset in range(0, size, 8):
            value = struct.unpack_from("<Q", table, offset)[0]
            target = value - base
            note = (loc(target) + " " + label(value)
                    if 0 <= target < pe.OPTIONAL_HEADER.SizeOfImage else "not an image VA")
            emit(f"+0x{offset:03x} VA=0x{value:016x} {note}")

    for rva in args.rva:
        f = owner(rva)
        if args.size is not None:
            start, end = rva, rva + args.size
        elif f:
            start, end = f[:2]
        else:
            raise SystemExit(f"No unwind function at {rva:x}; specify --size")
        emit("\nFUNCTION " + loc(start) + f" bytes=0x{end-start:x}")
        for ins in md.disasm(pe.get_data(start, end-start), base+start):
            emit(instruction(ins))

    text = "\n".join(lines) + "\n"
    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(text, encoding="utf-8")
        print(str(args.out.resolve()), len(lines), "lines")
    else:
        print(text, end="")


if __name__ == "__main__":
    main()
