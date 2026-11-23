#!/usr/bin/env python3
# Copyright (c) 2026 WebCpp.org
#
# Distributed under the Boost Software License, Version 1.0. (See
# accompanying file LICENSE_1_0.txt or copy at
# https://www.boost.org/LICENSE_1_0.txt)

"""Writes trystero's test vectors again from the published Trystero 0.27.0 packages.

Usage: vectors.py PREVIOUS OUTPUT

Runs `node record_0_27.mjs --dist node_modules PREVIOUS` beside this file, the recorder over the
packages package-lock.json pins (@trystero-p2p/core and @trystero-p2p/nostr 0.27.0), and writes
what it prints at OUTPUT. PREVIOUS is the committed fixture, test/fixtures/expected/
trystero-0.27.0.json, whose WebCrypto signatures the recorder keeps while 0.27.0 still verifies
them (design D19), so that the run writes the fixture again byte for byte exactly when the
published packages compute what the pinned source computed: webcpp.generated runs it as the check
generated-vectors of the oracle lane, and update-generated writes the fixture with it.

One member is not 0.27.0's: `recorded_with`, the Node that recorded the fixture, which the
recorder writes from the Node that runs it. The fixture's own value is kept, so that the check
compares what the packages compute on any Node, the CI's pinned one included.

The packages are installed first, as the oracle's other targets have them installed
(tools/node/install.py, which installs once per lockfile and lets runs at once wait for one
another), since webcpp.generated gives a generator no dependency on the install. Exit 0 when the
output is written, 1 when the install or the recorder fails, 2 on a usage error.
"""

from __future__ import annotations

import json
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
INSTALLER = HERE.parents[3] / 'tools/node/install.py'
# The recorder's own line for the member: one space of indentation, as JSON.stringify(out, null, 1)
# writes it, the second line of the file.
RECORDED_WITH = re.compile(r'^ "recorded_with": (".*"),$', re.MULTILINE)


def main(arguments: list[str]) -> int:
    if len(arguments) != 2:
        print('usage: vectors.py PREVIOUS OUTPUT', file=sys.stderr)
        return 2
    previous, output = (Path(a).resolve() for a in arguments)
    installed = subprocess.run([sys.executable, str(INSTALLER), str(HERE)], check=False)
    if installed.returncode != 0:
        print(f'vectors.py: {INSTALLER} failed', file=sys.stderr)
        return 1
    recorded = subprocess.run(
        ['node', 'record_0_27.mjs', '--dist', 'node_modules', str(previous)],
        cwd=HERE, check=False, stdout=subprocess.PIPE)
    if recorded.returncode != 0:
        print('vectors.py: record_0_27.mjs failed', file=sys.stderr)
        return 1
    text = recorded.stdout.decode('utf-8')
    kept = json.dumps(json.loads(previous.read_text(encoding='utf-8'))['recorded_with'])
    text, replaced = RECORDED_WITH.subn(lambda _: f' "recorded_with": {kept},', text, count=1)
    if replaced != 1:
        print('vectors.py: the recorder wrote no recorded_with line', file=sys.stderr)
        return 1
    output.write_text(text, encoding='utf-8', newline='')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
