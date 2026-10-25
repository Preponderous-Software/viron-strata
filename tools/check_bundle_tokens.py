"""Fails if the built page carries any JWT that is not read-only.

    python3 tools/check_bundle_tokens.py web-site/*

The page may carry exactly one kind of token: the public world's, whose scope
is `viron:read` alone (Viron answers 403 to any write made with it). Any other
JWT in the bundle -- a full-scope driver token, a UserAuth login, a token with
extra scopes or none -- is a credential that must never ship, and fails this
check. Standard library only.
"""

from __future__ import annotations

import base64
import json
import re
import sys

JWT = re.compile(rb"eyJ[A-Za-z0-9_-]{10,}\.[A-Za-z0-9_-]{10,}\.[A-Za-z0-9_-]{10,}")


def claims(token: bytes) -> dict:
    payload = token.split(b".")[1]
    payload += b"=" * (-len(payload) % 4)
    return json.loads(base64.urlsafe_b64decode(payload))


def problems(name: str, data: bytes) -> list[str]:
    out = []
    for m in JWT.finditer(data):
        try:
            c = claims(m.group(0))
        except Exception:  # noqa: BLE001 -- not decodable: treat as a credential
            out.append(f"{name}: an undecodable JWT-shaped string")
            continue
        scope = c.get("scope", c.get("scp"))
        scopes = set(scope.split()) if isinstance(scope, str) else set(scope or [])
        if scopes != {"viron:read"}:
            out.append(f"{name}: a JWT whose scope is {sorted(scopes) or 'missing'}, not exactly viron:read")
    return out


def main(paths: list[str]) -> int:
    found = []
    readonly = 0
    for p in paths:
        with open(p, "rb") as f:
            data = f.read()
        bad = problems(p, data)
        found += bad
        readonly += len(JWT.findall(data)) - len(bad)
    for line in found:
        print(line)
    print(f"{readonly} read-only token(s), {len(found)} other(s)")
    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
