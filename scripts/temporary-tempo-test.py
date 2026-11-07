"""Temporarily set 1.30x for diagnostics; restore only our own unchanged command."""
import json
import os
from pathlib import Path
import sys
import urllib.parse
import urllib.request

token = (Path(os.environ['LOCALAPPDATA']) / 'SpotifyPitchTempo/bridge-token.txt').read_text().strip()
headers = {'X-SPT-Token': token}
path = Path(__file__).resolve().parent.parent / 'reports/temporary-tempo-test-state.json'
keys = ('tempo', 'pitch', 'linked', 'enabled', 'reverb', 'distortion', 'muffle')

def request(endpoint, data=None):
    req = urllib.request.Request('http://127.0.0.1:43827' + endpoint,
                                 data=urllib.parse.urlencode(data).encode() if data else None,
                                 headers=headers)
    with urllib.request.urlopen(req, timeout=3) as response:
        return json.load(response)

if sys.argv[1] == 'start':
    if path.exists():
        raise SystemExit('A previous diagnostic state exists; resolve it first')
    old = request('/status')
    values = {key: old[key] for key in keys}
    if values != dict(tempo=1100, pitch=0, linked=1, enabled=1, reverb=0, distortion=0, muffle=0):
        raise SystemExit('Settings changed; did not start the test')
    path.write_text(json.dumps({'old': values, 'test': {**values, 'tempo': 1300}}))
    current = request('/config', {**values, 'tempo': 1300})
    print(json.dumps({'requested': 1300, 'sequence': current['commandSequence']}))
elif sys.argv[1] == 'restore':
    saved = json.loads(path.read_text())
    current = request('/status')
    if {key: current[key] for key in keys} == saved['test']:
        result = request('/config', saved['old'])
        print(json.dumps({'restored': saved['old'], 'sequence': result['commandSequence']}))
        path.unlink()
    else:
        print('Settings changed during the test; did not overwrite them')
else:
    raise SystemExit('Use start or restore')
