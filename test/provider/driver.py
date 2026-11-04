# Copyright (c) 2026 WebCpp.org
#
# Distributed under the Boost Software License, Version 1.0. (See
# accompanying file LICENSE_1_0.txt or copy at
# https://www.boost.org/LICENSE_1_0.txt)

"""Run the combined native Asio, Beast and Trystero provider consumer bounded."""

from pathlib import Path
import subprocess
import sys


def main() -> None:
    program = Path(sys.argv[1]).resolve()
    subprocess.run([str(program)], check=True, timeout=45)


if __name__ == '__main__':
    main()
