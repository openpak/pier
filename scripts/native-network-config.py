#!/usr/bin/env python3
"""Generate a native firmware network profile without changing the user's config."""
import argparse
from pathlib import Path
import yaml

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('source', type=Path)
parser.add_argument('destination', type=Path)
args = parser.parse_args()
config = yaml.safe_load(args.source.read_text())
core = config.setdefault('Core', {})
# Match requests must execute firmware, including its CXML/IPC plumbing.
native = {'libsysutil_np.sprx', 'libsysutil_np2.sprx', 'libsysutil.sprx'}
libs = [entry for entry in core.get('Libraries Control', [])
        if entry.rsplit(':', 1)[0] not in native]
core['Libraries Control'] = libs + [name + ':lle' for name in sorted(native)]
# Native firmware owns sign-in; neither fake PSN nor RPCN supplies guest state.
config.setdefault('Net', {})['PSN status'] = 'Disconnected'
args.destination.parent.mkdir(parents=True, exist_ok=True)
args.destination.write_text(yaml.safe_dump(config, sort_keys=False))
print(f'Native firmware network profile: {args.destination}')
