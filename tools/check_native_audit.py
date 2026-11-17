"""Check the audited inventory; optional pinned source trees verify exact coverage.
No downloads or runtime/provider loading. Run from any directory.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]

def clean(text):
    return re.sub(r'/\*.*?\*/|//[^\n]*', '', text, flags=re.S)

def options(path):
    body = re.search(r'(?:typedef\s+)?enum\s+SRT_SOCKOPT\s*\{(.*?)\}', clean(path.read_text()), re.S).group(1)
    return set(re.findall(r'\bSRTO_[A-Z0-9_]+\b', body)) - {'SRTO_E_SIZE', 'SRTO_DEPRECATED_END'}

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--robotweax', type=Path)
    parser.add_argument('--haivision', type=Path)
    args = parser.parse_args()
    if bool(args.robotweax) != bool(args.haivision):
        parser.error('supply both pinned source trees or neither')
    audit = json.loads((ROOT/'docs/design/extensions/native-surface.json').read_text())
    for kind in ['functions', 'options']:
        rows = audit[kind]
        names = [r['name'] for r in rows]
        assert len(names) == len(set(names)), f'duplicate {kind}'
        assert all(r['family'] and r['wrapper'] and (r['robotweax_declared'] or r['haivision_declared']) for r in rows)
    assert len([r for r in audit['functions'] if r['robotweax_declared']]) == 85
    assert len([r for r in audit['options'] if r['robotweax_declared']]) == 87
    assert len([r for r in audit['functions'] if r['haivision_declared']]) == 74
    assert len([r for r in audit['options'] if r['haivision_declared']]) == 61
    catalogue = (ROOT/'providers/metrics.inc').read_text()
    declared_metrics = re.findall(r'text\("native\.([A-Za-z0-9_]+)"\)', catalogue)
    source_fields = re.findall(r'm\.value\.(?:u64|i64|f64) = s\.([A-Za-z0-9_]+);', catalogue)
    assert declared_metrics == source_fields, 'metric mapping mismatch'
    assert len(declared_metrics) == len(set(declared_metrics)) == 82
    assert set(declared_metrics) == {r['name'] for r in audit['fields']['CBytePerfMon']}, 'native metric coverage'
    if args.robotweax:
        roots = {'robotweax': args.robotweax, 'haivision': args.haivision}
        for source in audit['sources']:
            path = roots[source['provider']]/source['path']
            assert hashlib.sha256(path.read_bytes()).hexdigest() == source['sha256'], f'source drift: {path}'
        expected = set((args.robotweax/'cmake/public-symbols.txt').read_text().split())
        assert expected == {r['name'] for r in audit['functions'] if r['robotweax_declared']}
        declaration = re.compile(r'\b(?:ROBOTWEAX_SRT_API|SRT_API)\b\s+(?:extern\s+)?[^;{}]*?\b((?:robotweax_srt|srt)_[A-Za-z0-9_]+)\s*(?=\(|\[|;)', re.S)
        for provider, path in [('robotweax',args.robotweax/'include/srt/srt.h'), ('haivision',args.haivision/'srtcore/srt.h')]:
            assert options(path) == {r['name'] for r in audit['options'] if r[provider+'_declared']}, f'option coverage: {provider}'
            declared = set(declaration.findall(clean(path.read_text())))
            if provider == 'robotweax':
                declared |= set(declaration.findall(clean((args.robotweax/'include/robotweax_srt.h').read_text())))
            assert declared == {r['name'] for r in audit['functions'] if r[provider+'_declared']}, f'symbol coverage: {provider}'
        assert {str(p.relative_to(args.robotweax)) for p in (args.robotweax/'include/robotweax/srt').rglob('*.hpp')} == {r['path'] for r in audit['source_cpp_headers']}
        for entry in audit['source_cpp_headers']:
            assert hashlib.sha256((args.robotweax/entry['path']).read_bytes()).hexdigest() == entry['sha256']
    print('Native audit inventory verified' + (' against both pinned source trees' if args.robotweax else ' (source trees not supplied)'))

if __name__ == '__main__':
    main()
