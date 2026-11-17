"""The application must use the facade, with no linked native SRT/UDT entry points."""
import re
import subprocess
import sys

symbols = subprocess.check_output([sys.argv[1], '-g', sys.argv[2]], text=True)
assert not re.search(r'(?:^|\s)_?srt_[A-Za-z0-9_]+|_?ZN3UDT', symbols, re.MULTILINE), 'Native SRT/UDT symbols linked into application'
print('Application links the multi-SRT host, with no native SRT/UDT entry points')
