#!/usr/bin/env python3
"""PencilKit queue and texture handoff regressions; host only."""
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[3]
for test in ('batch_plugin_test.py', 'pencil_plugin_test.py', 'shadow_blit_plugin_test.py'):
    subprocess.run([sys.executable, str(root / 'hakios-metal/tests' / test)], check=True)
