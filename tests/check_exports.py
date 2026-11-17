"""Check the actual dynamic export surface, not just source visibility flags."""
import subprocess
import sys

nm, *modules = sys.argv[1:]
for module in modules:
    flags = ["-gU"] if sys.platform == "darwin" else ["-D", "--defined-only"]
    output = subprocess.check_output([nm, *flags, module], text=True)
    symbols = {line.split()[-1] for line in output.splitlines() if line.strip()}
    expected = {"_srtm_provider_get_api" if sys.platform == "darwin" else "srtm_provider_get_api"}
    if symbols != expected:
        raise SystemExit(f"Unexpected exports in {module}: {sorted(symbols)}")
