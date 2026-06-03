#!/usr/bin/env python3
#
# disable_display.py — produce a board.dtb with the whole display/HDMI/audio
# subtree set to status="disabled", for headless EDK2 boot.
#
# WHY: EDK2's GOP owns DE3.0/mixer0. If the kernel's sunxi-drm/DE driver then
# probes the same block it faults ("Bug is in DE0, 0xff800000 not mapped") and
# floods the console. Disabling only DE/DRM exposes a second fault: a NULL-deref
# panic via wireplumber -> sunxi_hdmi_audio_set_info, because HDMI audio is
# welded to the HDMI display block. The fix is to disable the ENTIRE
# display+hdmi+audio subtree (sunxi-drm, de@, all tcon*, dsi*, edp*, hdmi*,
# hdmi_codec, panels) in the DTB that EDK2 hands to the kernel. The board's own
# /boot DTB is left untouched so a normal BSP boot keeps its panel.
#
# USAGE (on the board, which has dtc):
#   sudo cp /sys/firmware/fdt /tmp/live.dtb        # the live, fixed-up FDT
#   dtc -I dtb -O dts -o /tmp/live.dts /tmp/live.dtb
#   python3 disable_display.py /tmp/live.dts /tmp/board.dts
#   dtc -I dts -O dtb -o board.dtb /tmp/board.dts  # ~215 KB
#   # copy board.dtb to the SD ESP as \board.dtb (see README-SD-boot.md)
#
# Copyright (c) 2026, carpi-os contributors.
# SPDX-License-Identifier: BSD-2-Clause-Patent

import re
import sys

in_path  = sys.argv[1] if len(sys.argv) > 1 else '/tmp/live.dts'
out_path = sys.argv[2] if len(sys.argv) > 2 else '/tmp/board.dts'

lines = open(in_path).read().split('\n')

# node-name patterns whose whole subtree gets status="disabled".
patterns = [
    r'sunxi-drm', r'de@[0-9a-f]+', r'display-engine',
    r'tcon\d*@[0-9a-f]+', r'tcon-top\d*', r'tcon\d+',
    r'dsi\d*@[0-9a-f]+', r'edp\d*@[0-9a-f]+',
    r'hdmi\d*@[0-9a-f]+', r'hdmi_codec',
    r'dsi_panel@\d+', r'panel@\d+', r'disp@[0-9a-f]+',
]
nodere = re.compile(r'^(\s*)([\w,.+-]+(?:@[0-9a-fA-F]+)?)\s*\{')
statre = re.compile(r'^(\s*)status\s*=')
patre  = [re.compile('^(?:' + p + ')$') for p in patterns]

out = []
disabled = []
i = 0
n = len(lines)
while i < n:
    line = lines[i]
    m = nodere.match(line)
    if m and any(p.match(m.group(2)) for p in patre):
        indent, name = m.group(1), m.group(2)
        # walk this node body to its matching close brace
        out.append(line)
        depth = line.count('{') - line.count('}')
        j = i + 1
        body = []
        while j < n and depth > 0:
            depth += lines[j].count('{') - lines[j].count('}')
            if depth == 0:
                break
            body.append(lines[j])
            j += 1
        # replace an existing status property, else insert one FIRST
        # (DTS requires properties to precede subnodes).
        had = False
        for k, b in enumerate(body):
            if statre.match(b):
                body[k] = indent + '\tstatus = "disabled";'
                had = True
                break
        if not had:
            body.insert(0, indent + '\tstatus = "disabled";')
        out.extend(body)
        out.append(lines[j])   # closing brace
        disabled.append(name)
        i = j + 1
        continue
    out.append(line)
    i += 1

open(out_path, 'w').write('\n'.join(out))
print("Disabled subtrees:", disabled)
