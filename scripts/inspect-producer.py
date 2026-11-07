"""Find candidate renderer producer objects read-only for the verified Spotify build."""
import sys
from pathlib import Path

# Reuse the module/hash validation and read-only Windows bindings from the queue
# inspector, without invoking its scanner or retaining any audio samples.
source = Path(__file__).with_name('inspect-source-queue.py').read_text(encoding='utf-8')
prefix = source.split('try:\n    candidates = []', 1)[0]
pid = sys.argv[1]
sys.argv = [sys.argv[0], pid, '--output', str(Path(__file__).parent / '__unused__')]
exec(compile(prefix, 'inspect-source-queue.py', 'exec'))
try:
    needle = struct.pack('<Q', base + 0x1b16cf0)
    address = 0
    candidates = []
    while address < 0x7fffffffffff:
        region = Region()
        if not k.VirtualQueryEx(handle, address, c.byref(region), c.sizeof(region)):
            break
        region_base = region.base or 0
        end = region_base + region.bytes
        if end <= address:
            break
        if region.state == 0x1000 and region.type == 0x20000 and (region.protect & 0xff) in (4, 8, 0x40, 0x80) and not region.protect & 0x100:
            for offset in range(0, region.bytes, 1048576):
                try:
                    data = read(region_base + offset, min(1048576, region.bytes - offset))
                except OSError:
                    continue
                start = 0
                while True:
                    i = data.find(needle, start)
                    if i < 0:
                        break
                    start = i + 8
                    if i % 8:
                        continue
                    producer = region_base + offset + i
                    try:
                        adapter, supplier = u64(producer + 0x58), u64(producer + 0x60)
                        adapter_vtable, supplier_vtable = u64(adapter), u64(supplier)
                        if adapter_vtable != base + 0x1b166b8:
                            continue
                        source_interface = u64(supplier + 0x18)
                        filter_interface = u64(supplier + 8)
                        source_vtable = u64(source_interface)
                        filter_vtable = u64(filter_interface)
                        upstream_interface = u64(source_interface + 0x28)
                        upstream_vtable = u64(upstream_interface)
                        accounting_interface = u64(source_interface + 0x10)
                        accounting_vtable = u64(accounting_interface)
                        candidates.append({'producer': hex(producer), 'adapter': hex(adapter),
                                           'driver': hex(u64(adapter + 8)), 'supplier': hex(supplier),
                                           'supplier_vtable_rva': hex(supplier_vtable-base),
                                           'supplier_slot18_rva': hex(u64(supplier_vtable+0x18)-base),
                                           'source_vtable_rva': hex(source_vtable-base),
                                           'source_slot18_rva': hex(u64(source_vtable+0x18)-base),
                                           'upstream_vtable_rva': hex(upstream_vtable-base),
                                           'upstream_slot18_rva': hex(u64(upstream_vtable+0x18)-base),
                                           'accounting_vtable_rva': hex(accounting_vtable-base),
                                           'accounting_slot38_rva': hex(u64(accounting_vtable+0x38)-base),
                                           'filter_vtable_rva': hex(filter_vtable-base),
                                           'filter_slot28_rva': hex(u64(filter_vtable+0x28)-base)})
                    except (OSError, ValueError):
                        pass
        address = end
    print(json.dumps({'readOnly': True, 'candidates': candidates}, indent=2))
finally:
    k.CloseHandle(handle)
