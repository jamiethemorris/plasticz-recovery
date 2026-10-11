#!/usr/bin/env python3
"""Trace original editor's integer geometry with explicit GUI-constructor stubs.

Only instructions in editor-open 0x150a4..0x16bb1 run. Every GUI call is logged
and skipped; bitmap sizes come from the shipped PNG IHDRs. This is constructor
argument recovery, not a running legacy UI or evidence of mouse behavior.
"""
import hashlib
import json
from pathlib import Path
import re
import struct

from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_EBX, UC_X86_REG_EIP, UC_X86_REG_ESP
from x86_oracle import X86Oracle, ROOT, STACK_TOP, SOURCE_SHA256

RESOURCES = Path('/Library/Audio/Plug-Ins/VST/PlastiCZ.vst/Contents/Resources')
HEAP = 0x3000000
CTOR = {0x3d62: 'frame', 0x26dd0: 'context_menu', 0x275b2: 'multiframe_control',
        0xdf24: 'text_edit', 0x27ac6: 'option_menu', 0x26ca8: 'momentary_button',
        0x2db72: 'meter', 0xe112: 'animated_control', 0x2e13a: 'vertical_slider',
        0x2e9a0: 'horizontal_slider'}
BITMAP_ARGS = {0x3d62: [], 0x26dd0: [4], 0x275b2: [4], 0xdf24: [5],
               0x27ac6: [4], 0x26ca8: [4], 0x2db72: [4], 0xe112: [6],
               0x2e13a: [6, 7], 0x2e9a0: [6, 7]}


class LayoutOracle(X86Oracle):
    def initialize_cosine_table(self):
        # Geometry needs only the verified Mach-O loader. Do not execute the
        # superclass's unrelated DSP cosine initializer or imported cosf.
        pass


def main():
    oracle = LayoutOracle()
    uc = oracle.uc
    uc.mem_map(HEAP, 0x200000)
    # Calls never enter this mock vtable. Its readable addresses let the
    # original constructor load method pointers before our call-site hooks.
    uc.mem_write(HEAP, struct.pack('<256I', *([HEAP + 0x800] * 256)))
    cursor = HEAP + 0x1000
    def allocate(size):
        nonlocal cursor
        result = cursor
        cursor += (max(size, 4) + 15) & ~15
        assert cursor < HEAP + 0x200000
        uc.mem_write(result, struct.pack('<I', HEAP))
        return result
    editor = allocate(0x400)
    bitmaps = {}
    resources = []
    for path in sorted(RESOURCES.glob('bmp*.png')):
        data = path.read_bytes()
        width, height = struct.unpack('>II', data[16:24])
        resources.append(dict(id=int(path.stem[3:]), file=path.name, width=width, height=height,
                              sha256=hashlib.sha256(data).hexdigest()))
    resource_by_id = {r['id']: r for r in resources}
    assembly = {}
    for line in (ROOT / 'analysis/disassembly-i386.txt').read_text().splitlines():
        match = re.match(r'([0-9a-f]{8})\s+(.+)', line)
        if match: assembly[int(match[1], 16)] = match[2]
    controls, objects, pages, calls, errors = [], {}, [], [], []
    def words(address, count): return struct.unpack('<' + 'I' * count, uc.mem_read(address, 4 * count))
    def string(address):
        data = bytearray()
        while address and len(data) < 256:
            byte = bytes(uc.mem_read(address, 1))
            if byte == b'\0': break
            data.extend(byte)
            address += 1
        return data.decode('latin1')
    def hook(_uc, address, size, _):
        if address == 0x16bb1:
            uc.emu_stop()
            return
        instruction = assembly.get(address, '')
        if not instruction.startswith('calll'): return
        args = words(uc.reg_read(UC_X86_REG_ESP), 10)
        direct = re.match(r'calll\s+(0x[0-9a-f]+)', instruction)
        result = 0
        if direct:
            target = int(direct[1], 16)
            calls.append(dict(address=hex(address), target=hex(target)))
            if target == 0x38f18:
                uc.reg_write(UC_X86_REG_EBX, address + size)
            elif target == 0x3ed02:
                result = allocate(args[0])
            elif target == 0x375e:
                item = resource_by_id[args[1]]
                bitmaps[args[0]] = args[1]
                uc.mem_write(args[0] + 12, struct.pack('<II', item['width'], item['height']))
            elif target == 0x27f50:
                pages.append(args[0])
            elif target in CTOR:
                rect = list(struct.unpack('<4i', uc.mem_read(args[1], 16)))
                item = dict(constructor=hex(target), callsite=hex(address), kind=CTOR[target],
                            rect_ltrb=rect, raw_arguments=list(args),
                            tag=args[3] if target != 0x3d62 else None, bitmap_ids=[])
                for index in BITMAP_ARGS[target]:
                    value = args[index]
                    if value in bitmaps: item['bitmap_ids'].append(dict(argument=index, id=bitmaps[value]))
                if target == 0x275b2:
                    item['frame_count'] = args[5]
                if target in (0x2e13a, 0x2e9a0):
                    item['handle_min'] = args[4]
                    item['handle_max'] = args[5]
                controls.append(item)
                objects[args[0]] = item
            elif target == 0x69064:
                uc.mem_write(args[0], bytes(uc.mem_read(args[1], args[2])))
                result = args[0]
            elif target not in (0x196a, 0xe64a):
                errors.append(f'Unexpected direct call {target:#x} at {address:#x}')
                uc.emu_stop()
        else:
            offset_match = re.search(r'\*(0x[0-9a-f]+)\(', instruction)
            offset = int(offset_match[1], 16) if offset_match else None
            if offset == 8 and args[0] in pages and args[1] in objects:
                objects[args[1]]['page'] = pages.index(args[0])
            if args[0] in objects:
                item = objects[args[0]]
                if offset == 0x12c:
                    item.setdefault('menu_items', []).append(dict(index=args[2], label=string(args[1]), string_address=hex(args[1])))
                if offset == 0xd8 and item['kind'] == 'multiframe_control':
                    item['source_crop_origin'] = list(struct.unpack('<2i', uc.mem_read(args[1], 8)))
                    item['source_crop_callsite'] = hex(address)
                if offset == 0xfc and item['kind'] == 'context_menu':
                    labels = []
                    for index in range(32):
                        pointer = words(args[1] + index * 4, 1)[0]
                        if not pointer: break
                        labels.append(dict(index=index, label=string(pointer), string_address=hex(pointer)))
                    else: raise ValueError('Unterminated context-menu pointer array')
                    item['menu_items'] = labels
                if offset in (0x100, 0x104):
                    item.setdefault('properties', []).append(dict(method_offset=hex(offset), value_bits=hex(args[1]),
                        float_value=struct.unpack('<f', struct.pack('<I', args[1]))[0], callsite=hex(address)))
        uc.reg_write(UC_X86_REG_EAX, result)
        uc.reg_write(UC_X86_REG_EIP, address + size)
    uc.hook_add(UC_HOOK_CODE, hook, begin=0x150a4, end=0x16cd8)
    oracle._reset_registers(0x037f)
    uc.mem_write(STACK_TOP, struct.pack('<III', 0, editor, 0))
    uc.emu_start(0x150a4, 0x16bb1, count=100000)
    assert not errors, errors
    assert uc.reg_read(UC_X86_REG_EIP) == 0x16bb1
    for offset in range(0x28, 0x220, 4):
        pointer = words(editor + offset, 1)[0]
        if pointer in objects:
            objects[pointer].setdefault('editor_field_offsets', []).append(hex(offset))
    for item in controls:
        # Raw mock addresses are retained for replay audit, not pointer identity
        # in a real Carbon process.
        item['rect_xywh'] = [*item['rect_ltrb'][:2], item['rect_ltrb'][2] - item['rect_ltrb'][0],
                            item['rect_ltrb'][3] - item['rect_ltrb'][1]]
        if item['tag'] is not None and 0 <= item['tag'] < 38:
            item['parameter_index'] = item['tag']
        if item['tag'] is not None and 53 <= item['tag'] <= 84:
            index = item['tag'] - 53
            item['parameter_index'] = 8 + index // 8
            item['wave_index'] = index % 8
            item['parameter_on_value'] = struct.unpack('<f', struct.pack('<f', (index % 8) * 0.1428571492433548))[0]
            x, y = item['source_crop_origin']
            item['source_crops_xywh'] = [[x, y, 16, 16], [x, y + 36, 16, 16]]
        if item['kind'] == 'vertical_slider':
            item['direction'] = '0=bottom, 1=top; draw 0x2dcc7..0x2dd16 uses (1-value)'
    assert len(controls) == 86
    waves = [c for c in controls if 'wave_index' in c]
    assert len(waves) == 32
    for item in waves:
        index, group = item['wave_index'], (item['parameter_index'] - 8) % 2
        assert item['source_crop_origin'] == [20 * (index % 4) + 100 * group, 20 * (index // 4)]
    assert {c['parameter_index'] for c in controls if 'parameter_index' in c} == set(range(38)) - {6, 7, 19, 26, 27}
    report = dict(protocol='plasticz-editor-layout-trace-v1', source_sha256=SOURCE_SHA256,
                  extractor_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                  dependencies_sha256={str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest()
                                       for path in [ROOT / 'scripts/x86_oracle.py',
                                                    ROOT / 'analysis/disassembly-i386.txt']},
                  entry='0x150a4', stopped_before='0x16bb1',
                  scope='Original integer control-construction geometry with all GUI calls stubbed; no legacy GUI, host, or license execution.',
                  limitations=['Mock GUI constructors only supply bitmap sizes and readable object vtables.',
                               'Control rectangles are constructor arguments, not observed drawn bounds.',
                               'Mouse semantics and OS behavior are not executed.',
                               'Stops before editor page switch and parameter refresh; those require static analysis.'],
                  resources=resources, controls=controls, direct_calls=calls)
    output = ROOT / 'analysis/recovered/original-ui-layout.json'
    output.write_text(json.dumps(report, indent=2) + '\n')
    print(f'Recovered {len(controls)} control constructions into {output}')
    for item in controls:
        print(item['kind'], item['tag'], item['rect_xywh'], item.get('page'), item['bitmap_ids'])


if __name__ == '__main__': main()
