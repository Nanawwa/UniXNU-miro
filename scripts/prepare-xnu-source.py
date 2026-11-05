#!/usr/bin/env python3
"""Restore omitted DriverKit headers from Apple's earlier Darwin 23 source.

Device override of the base script. It is byte-for-byte the base logic except for
one host dependency that breaks on networks that block
raw.githubusercontent.com with a TLS reset (common on mainland China ISPs, and the
only reason this board could not run `unixnu build kernel` on Linux):

  * the listing fetches still use api.github.com (reachable here),
  * the individual header downloads fall back, in order, to
    (1) the direct URL, (2) each proxy prefix in UNIXNU_RAW_MIRRORS,
    (3) api.github.com/repos/O/R/git/blobs/SHA (base64).

The header set, the git-blob verification, the cache layout and the restored-headers
record are unchanged, so the base script can replace this file again the moment the
network reaches raw.githubusercontent.com.

Nothing here belongs in UniXNU (read only): it lives in the device repository and the
tree assembler copies it over the base script of the same name.
"""
import base64
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import urllib.parse
import urllib.request

# Tags of the Apple sources that still carry the headers newer xnu omits; feeds.toml sets them for the unixnu driver.
TAG = os.environ.get('UNIXNU_DRIVERKIT_TAG', 'xnu-10063.121.3')
TRUST_TAG = os.environ.get('UNIXNU_TRUSTCACHE_TAG', 'xnu-8796.101.5')
DESTINATION = Path('.cache/xnu-work/iokit/DriverKit')
CACHE = Path(f'.cache/sources/xnu-driverkit-{TAG.removeprefix("xnu-")}.json')
URL = f'https://api.github.com/repos/apple-oss-distributions/xnu/contents/iokit/DriverKit?ref={TAG}'
# Anonymous GitHub API calls share a small per-address quota that hosted CI runners exhaust (HTTP 403);
# a token in GITHUB_TOKEN or GH_TOKEN is sent to GitHub hosts only.
TOKEN = os.environ.get('GITHUB_TOKEN') or os.environ.get('GH_TOKEN')

# Prefixes that proxy a raw.githubusercontent.com URL, tried only when the direct
# host fails. Override or reorder with a comma-separated UNIXNU_RAW_MIRRORS.
RAW_MIRRORS = [m.strip() for m in os.environ.get(
    'UNIXNU_RAW_MIRRORS',
    'https://ghproxy.net/,https://gh-proxy.com/,https://ghfast.top/').split(',') if m.strip()]


def _read(url):
    request = urllib.request.Request(url, headers={'User-Agent': 'unixnu-prepare-xnu-source'})
    if TOKEN and urllib.parse.urlsplit(url).hostname in ('api.github.com', 'raw.githubusercontent.com'):
        request.add_header('Authorization', f'Bearer {TOKEN}')
    with urllib.request.urlopen(request, timeout=30) as response:
        return response.read()


def _blob_from_api(url, sha):
    """The same git-blob bytes through api.github.com, for hosts that cannot reach
    raw.githubusercontent.com. Returns None when the URL is not a raw blob URL."""
    parts = urllib.parse.urlsplit(url).path.strip('/').split('/', 3)
    if len(parts) != 4 or not sha:
        return None
    owner, repo, _, _ = parts
    payload = json.loads(_read(f'https://api.github.com/repos/{owner}/{repo}/git/blobs/{sha}'))
    return base64.b64decode(payload['content'])


def fetch(url, sha=None):
    if urllib.parse.urlsplit(url).hostname != 'raw.githubusercontent.com':
        return _read(url)
    # Blob API first: on networks where raw.githubusercontent.com is silently
    # dropped (SYN never answered), trying raw first burns 30 s per header.
    # Mirrors remain the fallback for when the blob API is rate-limited.
    data = _blob_from_api(url, sha)
    if data is not None:
        return data
    for candidate in [mirror + url for mirror in RAW_MIRRORS]:
        try:
            return _read(candidate)
        except Exception:
            continue
    raise RuntimeError(f'cannot fetch {url}')


def restore(entry, destination=DESTINATION, tag=TAG):
    if entry['type'] != 'file' or not entry['name'].endswith('.h'):
        return None
    name = entry.get('relative', entry['name'])
    target = destination / name
    if target.exists():
        return None
    data = fetch(entry['download_url'], entry.get('sha'))
    digest = hashlib.sha1(b'blob ' + str(len(data)).encode() + b'\0' + data).hexdigest()
    if digest != entry['sha']:
        raise ValueError(f'Git blob hash mismatch: {entry["name"]}')
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(data)
    return {'name': name, 'source_tag': tag, 'git_blob': digest,
            'sha256': hashlib.sha256(data).hexdigest()}


if __name__ == '__main__':
    if not DESTINATION.is_dir():
        raise SystemExit('Create the local XNU work source before preparing headers')
    if not CACHE.exists():
        CACHE.parent.mkdir(parents=True, exist_ok=True)
        CACHE.write_bytes(fetch(URL))
    entries = json.loads(CACHE.read_text())
    crypto_cache = CACHE.with_name(f'xnu-driverkit-crypto-{TAG.removeprefix("xnu-")}.json')
    if not crypto_cache.exists():
        crypto_url = URL.replace('/DriverKit?', '/DriverKit/crypto?')
        crypto_cache.write_bytes(fetch(crypto_url))
    for entry in json.loads(crypto_cache.read_text()):
        entry['relative'] = 'crypto/' + entry['name']
        entries.append(entry)
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as executor:
        restored = [entry for entry in executor.map(restore, entries) if entry]
        trust_tag = TRUST_TAG
        trust_cache = CACHE.with_name(f'xnu-trustcache-{TRUST_TAG.removeprefix("xnu-")}.json')
        if not trust_cache.exists():
            url = ('https://api.github.com/repos/apple-oss-distributions/xnu/'
                   f'contents/EXTERNAL_HEADERS/TrustCache?ref={trust_tag}')
            trust_cache.write_bytes(fetch(url))
        trust_destination = Path('.cache/xnu-work/EXTERNAL_HEADERS/TrustCache')
        def restore_trust(entry):
            return restore(entry, trust_destination, trust_tag)
        restored.extend(entry for entry in executor.map(
            restore_trust, json.loads(trust_cache.read_text())) if entry)
    previous = Path('.cache/sources/xnu-restored-headers.json')
    existing = json.loads(previous.read_text()) if previous.exists() else []
    by_name = {entry['name']: entry for entry in existing + restored}
    previous.write_text(json.dumps(list(by_name.values()), indent=2) + '\n')
    print(f'Restored {len(restored)} omitted public headers from Apple; blob hashes verified')