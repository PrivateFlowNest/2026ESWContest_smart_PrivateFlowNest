"""Pairing / token administration for the Python Mobile API (same flags as the C++ homemind-mobile-api).

Run by a person on the machine that runs the API, with the same environment (HOMEMIND_DATA_DIR, ...):

    python -m app.cli --print-pairing-qr [--base-url URL] [--ttl SECONDS] [--json]
    python -m app.cli --list-tokens
    python -m app.cli --revoke-token ID | --revoke-all-tokens

The pairing code is printed to this terminal only, never logged.
"""

from __future__ import annotations

import argparse
import json
import sys
import time

from .auth import AuthConfig, PairingService, utc_iso_seconds


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="python -m app.cli", description=__doc__.splitlines()[0])
    action = parser.add_mutually_exclusive_group(required=True)
    action.add_argument("--print-pairing-qr", action="store_true", help="print a one-time pairing code and QR payload")
    action.add_argument("--list-tokens", action="store_true", help="list paired phones (id, created, name)")
    action.add_argument("--revoke-token", metavar="ID", help="revoke one token by id")
    action.add_argument("--revoke-all-tokens", action="store_true", help="revoke every token")
    parser.add_argument("--base-url", default="", help="URL the phone uses (default HOMEMIND_PUBLIC_BASE_URL)")
    parser.add_argument("--ttl", type=int, default=None, help="code lifetime in seconds (30..3600)")
    parser.add_argument("--json", action="store_true", help="print only the QR payload JSON")
    args = parser.parse_args(argv)

    service = PairingService(AuthConfig.from_env())
    if args.print_pairing_qr:
        service.prepare()
        try:
            offer = service.make_offer(int(time.time()), args.base_url or service.config.public_base_url,
                                       args.ttl if args.ttl is not None else service.config.code_ttl_s)
        except ValueError as error:
            print(error, file=sys.stderr)
            return 2
        payload = json.dumps(offer.qr, ensure_ascii=False, separators=(",", ":"))
        if args.json:
            print(payload)
            return 0
        print(f"HomeMind pairing (one-time, valid until {utc_iso_seconds(offer.expires_at_s)} UTC)")
        print(f"  device:       {service.config.device_name} ({service.config.device_id})")
        print(f"  base URL:     {offer.qr['base_url']}")
        print(f"  pairing code: {offer.code}   <- compare with the code the app shows")
        print("QR payload (render it locally, e.g. qrencode -t ANSIUTF8; never in an online QR generator):")
        print(payload)
        return 0
    if args.list_tokens:
        records = service.tokens.list()
        if not records:
            print("no paired phones")
        for record in records:
            print(f"{record.id}  {record.created_at}  {record.device_name}")
        return 0
    if args.revoke_token:
        if not service.tokens.revoke(args.revoke_token):
            print(f"no token with id {args.revoke_token}", file=sys.stderr)
            return 1
        print(f"revoked {args.revoke_token}")
        return 0
    print(f"revoked {service.tokens.revoke_all()} token(s)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
