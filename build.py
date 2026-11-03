"""Build using the sibling Smartwatch SDK; accepts --smartwatch-root PATH."""
from pathlib import Path
import argparse
import sys

project=Path(__file__).resolve().parent
probe=argparse.ArgumentParser(add_help=False)
probe.add_argument('--smartwatch-root',type=Path)
known,_=probe.parse_known_args()
candidates=[known.smartwatch_root] if known.smartwatch_root else [project.parent/name for name in ('smart_watch-main','Smartwatch','smart_watch')]
sdk=next((p.resolve() for p in candidates if p and (p/'tools/device_build.py').is_file()),None)
if sdk is None:raise SystemExit('Current Smartwatch SDK not found; use --smartwatch-root PATH (tools/device_build.py required)')
sys.path.insert(0,str(sdk/'tools'))
from device_build import main
main(project,'DeviceBoot',sdk)
