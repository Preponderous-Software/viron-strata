"""Mints an HS256 JWT that a *local, throwaway* Viron accepts.

    python3 tools/mint_dev_token.py <secret-file> [issuer]

Viron validates UserAuth tokens with a shared HMAC secret (JWT_SECRET) and an
`iss` claim (JWT_ISSUER, default "userauth"). For a Viron you started yourself
with a secret you chose, this stands in for UserAuth. Never point it at a real
deployment's secret. The token lasts one day.
"""

from __future__ import annotations

import base64
import hashlib
import hmac
import json
import sys
import time


def b64(data: bytes) -> str:
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode()


def main() -> int:
    secret = open(sys.argv[1]).read().strip().encode()
    issuer = sys.argv[2] if len(sys.argv) > 2 else "userauth"
    now = int(time.time())
    header = b64(json.dumps({"alg": "HS256", "typ": "JWT"}).encode())
    payload = b64(json.dumps({"sub": "viron-strata-dev", "iss": issuer, "iat": now, "exp": now + 86400}).encode())
    sig = b64(hmac.new(secret, f"{header}.{payload}".encode(), hashlib.sha256).digest())
    print(f"{header}.{payload}.{sig}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
