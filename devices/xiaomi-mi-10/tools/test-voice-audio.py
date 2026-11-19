#!/usr/bin/env python3
"""Actual voice APR, DMA, passive sessions and route ownership under sanitizers."""
from pathlib import Path
import re
import subprocess

device = Path(__file__).resolve().parents[1]
root = device.parents[1]
dev = device / 'overlays/xnu/files/bsd/dev'
out = root / 'out/xiaomi-mi-10/voice/tests'
out.mkdir(parents=True, exist_ok=True)
# Cache maintenance needs privileged ARM instructions. The model checks PTEs,
# ordering of lock/flush writes and ownership; hardware validates cache/TLB DMA.
memory = (dev / 'umi_voice_audio_memory.inc').read_text()
memory = re.sub(r'__asm__ volatile\("[^"\n]+"[^;]*;', '(void)0;', memory)
assert '__asm__' not in memory
(out / 'umi_voice_audio_memory_host.inc').write_text(memory)
for name in ('audio-apr', 'audio-memory', 'audio-route', 'audio-receiver'):
    binary = out / name
    subprocess.run(['clang', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-I' + str(dev), '-I' + str(out),
                    str(device / 'tools/voice-tests' / (name + '.c')), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)

subprocess.run(["python3", str(device / "tools/test-voice.py")], check=True)
