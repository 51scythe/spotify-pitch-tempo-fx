"""Read-only, build-bound Spotify source-queue diagnostics. Does not retain PCM.

Uses known snapshot-derived object layouts, validated against the live module
and format. Concurrent queue samples are estimates, not atomic playback events.
"""
import argparse
import ctypes as c
import hashlib
import json
import os
from pathlib import Path
import struct
import time

parser = argparse.ArgumentParser(__doc__)
parser.add_argument('pid', type=int)
parser.add_argument('--seconds', type=float, default=20)
parser.add_argument('--renderer', type=lambda s: int(s, 0))
parser.add_argument('--interval', type=float, default=.01)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
if args.output.exists():
    raise SystemExit('Output already exists')
k = c.WinDLL('kernel32', use_last_error=True)
k.OpenProcess.argtypes = [c.c_uint32, c.c_int, c.c_uint32]
k.OpenProcess.restype = c.c_void_p
k.ReadProcessMemory.argtypes = [c.c_void_p, c.c_void_p, c.c_void_p, c.c_size_t, c.POINTER(c.c_size_t)]
k.VirtualQueryEx.argtypes = [c.c_void_p, c.c_void_p, c.c_void_p, c.c_size_t]
k.VirtualQueryEx.restype = c.c_size_t
k.CloseHandle.argtypes = [c.c_void_p]
k.CreateToolhelp32Snapshot.argtypes = [c.c_uint32, c.c_uint32]
k.CreateToolhelp32Snapshot.restype = c.c_void_p

class Module(c.Structure):
    _fields_ = [('size', c.c_uint32), ('id', c.c_uint32), ('pid', c.c_uint32),
               ('globalUsage', c.c_uint32), ('processUsage', c.c_uint32),
               ('base', c.c_void_p), ('bytes', c.c_uint32), ('handle', c.c_void_p),
               ('name', c.c_wchar * 256), ('path', c.c_wchar * 260)]

class Region(c.Structure):
    _fields_ = [('base', c.c_void_p), ('allocationBase', c.c_void_p),
               ('allocationProtect', c.c_uint32), ('partition', c.c_uint16),
               ('bytes', c.c_size_t), ('state', c.c_uint32), ('protect', c.c_uint32),
               ('type', c.c_uint32)]

k.Module32FirstW.argtypes = k.Module32NextW.argtypes = [c.c_void_p, c.POINTER(Module)]
snapshot = k.CreateToolhelp32Snapshot(0x18, args.pid)
module = Module(); module.size = c.sizeof(module)
modules = {}
try:
    ok = k.Module32FirstW(snapshot, c.byref(module))
    while ok:
        modules[module.name.lower()] = (module.base, module.path)
        ok = k.Module32NextW(snapshot, c.byref(module))
finally:
    k.CloseHandle(snapshot)
base, path = modules['spotify.dll']
expected = Path(os.environ['APPDATA']) / 'Spotify/Spotify.dll'
if Path(path).resolve() != expected.resolve():
    raise SystemExit('Unexpected Spotify module path')
digest = hashlib.sha256(Path(path).read_bytes()).hexdigest()
if digest != '93ba5ee486e148114a1d6db388cce318b04d2a011376ff6301f5ee0dea432a12':
    raise SystemExit('Unsupported Spotify binary')
handle = k.OpenProcess(0x410, False, args.pid)  # query + read, never write
if not handle:
    raise SystemExit('OpenProcess failed')

def read(address, size):
    if not address or size < 0 or size > 1048576:
        raise ValueError('Invalid read')
    buffer = c.create_string_buffer(size)
    count = c.c_size_t()
    if not k.ReadProcessMemory(handle, address, buffer, size, c.byref(count)) or count.value != size:
        raise OSError(c.get_last_error(), 'ReadProcessMemory failed')
    return buffer.raw

def u64(address):
    return struct.unpack('<Q', read(address, 8))[0]

def validate(address):
    try:
        if u64(address) != base + 0x1b02158:
            return None
        data = read(address + 0x1554, 0x114)
        tag, channels, rate, _, alignment, bits = struct.unpack_from('<HHIIHH', data)
        capacity = struct.unpack_from('<I', data, 0x28)[0]
        source = struct.unpack_from('<Q', data, 0x3c)[0]
        if channels != 2 or not 8000 <= rate <= 192000 or not 0 < capacity <= rate * 2 or bits != 32 or alignment != 8:
            return None
        if u64(source) != base + 0x1b01a70:
            return None
        queue = u64(source + 0x20)
        if u64(queue) != base + 0x1b01860:
            return None
        return dict(renderer=hex(address), source=hex(source), queue=hex(queue),
                    sampleRate=rate, channels=channels, bits=bits, tag=tag, capacity=capacity)
    except (OSError, ValueError):
        return None

try:
    candidates = []
    scanned = 0
    if args.renderer:
        candidate = validate(args.renderer)
        if candidate:
            candidates.append(candidate)
    else:
        address = 0
        needle = struct.pack('<Q', base + 0x1b02158)
        while address < 0x7fffffffffff:
            region = Region()
            if not k.VirtualQueryEx(handle, address, c.byref(region), c.sizeof(region)):
                break
            regionBase = region.base or 0
            end = regionBase + region.bytes
            if end <= address:
                break
            if region.state == 0x1000 and region.type == 0x20000 and (region.protect & 0xff) in (4, 8, 0x40, 0x80) and not region.protect & 0x100:
                for offset in range(0, region.bytes, 1048576):
                    size = min(1048576, region.bytes - offset)
                    try:
                        data = read(regionBase + offset, size)
                    except OSError:
                        continue
                    scanned += len(data)
                    start = 0
                    while True:
                        index = data.find(needle, start)
                        if index < 0:
                            break
                        if index % 8 == 0:
                            candidate = validate(regionBase + offset + index)
                            if candidate:
                                candidates.append(candidate)
                        start = index + 8
            address = end
    print(json.dumps({'candidates': candidates, 'bytesScanned': scanned}), flush=True)
    if len(candidates) != 1:
        raise SystemExit('Expected one validated renderer; supply --renderer to select')
    candidate = candidates[0]
    queue = int(candidate['queue'], 16) - 8  # float-interface thunk adjusts this
    consumedTarget = u64(queue + 0x30)
    consumedVtable = u64(consumedTarget)
    candidate['consumedTarget'] = hex(consumedTarget)
    candidate['consumedVtableRva'] = hex(consumedVtable - base)
    candidate['consumedMethodRva'] = hex(u64(consumedVtable + 0xb8) - base)
    print(json.dumps({'upstream': candidate}), flush=True)
    rows = []
    previousChunks = set()
    observedEnqueuedSamples = 0
    observedNewChunks = 0
    start = time.perf_counter()
    while time.perf_counter() - start < args.seconds:
        row = {'seconds': time.perf_counter() - start}
        try:
            state = read(queue + 0x18, 0x98)
            head, tail = struct.unpack_from('<QQ', state)
            row['empty'] = head == tail
            row['running'] = bool(state[0x10])
            row['underrunEnabled'] = bool(state[0x11])
            row['initializedRateCode'] = struct.unpack_from('<I', state, 0x5c)[0]
            row['starved'] = bool(state[0x8b])
            row['markers'] = struct.unpack_from('<q', state, 0x90)[0]
            samples = nodes = 0
            seen = set()
            currentChunks = set()
            while head and head != tail and nodes < 512 and head not in seen:
                seen.add(head)
                node = read(head, 0x28)
                chunk = struct.unpack_from('<Q', node)[0]
                if chunk:
                    remaining = u64(chunk + 0x20)
                    if remaining > candidate['sampleRate'] * 120:
                        raise ValueError('Unexpected audio chunk length')
                    samples += remaining
                    # Compare consecutive snapshots to handle allocator reuse.
                    # This misses chunks consumed entirely between samples and
                    # can be affected by concurrent list mutation.
                    identity = (head, chunk)
                    currentChunks.add(identity)
                    if identity not in previousChunks:
                        observedEnqueuedSamples += remaining
                        observedNewChunks += 1
                head = struct.unpack_from('<Q', node, 0x20)[0]
                nodes += 1
            row.update(queuedFrames=samples // 2, nodes=nodes, truncated=head != tail,
                       observedEnqueuedFrames=observedEnqueuedSamples // 2,
                       observedNewChunks=observedNewChunks)
            previousChunks = currentChunks
        except (OSError, ValueError) as error:
            row['error'] = str(error)
        rows.append(row)
        time.sleep(max(.001, args.interval))
    result = {'pid': args.pid, 'at': time.strftime('%Y-%m-%dT%H:%M:%S'), 'sha256': digest,
              'candidate': candidate, 'readOnly': True, 'audioRetained': False,
              'concurrentSamplesNotAtomic': True, 'rows': rows}
    args.output.write_text(json.dumps(result, indent=2), encoding='utf-8')
    valid = [r for r in rows if 'queuedFrames' in r]
    print(json.dumps({'output': str(args.output), 'samples': len(rows), 'valid': len(valid),
                      'emptySamples': sum(r['empty'] for r in valid),
                      'starvedSamples': sum(r['starved'] for r in valid),
                      'minQueuedFrames': min((r['queuedFrames'] for r in valid), default=None),
                      'maxQueuedFrames': max((r['queuedFrames'] for r in valid), default=None)}, indent=2))
finally:
    k.CloseHandle(handle)
