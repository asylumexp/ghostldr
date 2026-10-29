#!/usr/bin/env python3
"""BearSSL trust anchors from PEM certificates (ghostldr/native/plansync).

    python3 ghostldr/tools/gen_trust_anchors.py                # roots.pem -> plansync_ta.c
    python3 ghostldr/tools/gen_trust_anchors.py --check        # exit 1 if plansync_ta.c is stale

Each certificate becomes a br_x509_trust_anchor: its subject name (DER) and
its RSA or EC public key, as BearSSL's `brssl ta` writes them.  Only the
parts of DER a certificate needs are read here: no third-party modules.
"""

from __future__ import annotations

import argparse
import base64
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "ghostldr/native/plansync/roots.pem"
OUT = ROOT / "ghostldr/native/plansync/plansync_ta.c"

OID_RSA = bytes.fromhex("2a864886f70d010101")
OID_EC = bytes.fromhex("2a8648ce3d0201")
CURVES = {bytes.fromhex("2a8648ce3d030107"): "BR_EC_secp256r1", bytes.fromhex("2b81040022"): "BR_EC_secp384r1",
          bytes.fromhex("2b81040023"): "BR_EC_secp521r1"}


def tlv(d: bytes, at: int) -> tuple[int, int, int]:
    """(tag, start of the value, end of the value) of the DER element at `at`."""
    tag, n = d[at], d[at + 1]
    at += 2
    if n & 0x80:
        k, n = n & 0x7F, 0
        for _ in range(k):
            n, at = n << 8 | d[at], at + 1
    return tag, at, at + n


def children(d: bytes, start: int, end: int) -> list[tuple[int, int, int, int]]:
    """(tag, element start, value start, value end) of each element in [start, end)."""
    out, at = [], start
    while at < end:
        tag, vs, ve = tlv(d, at)
        out.append((tag, at, vs, ve))
        at = ve
    return out


def anchor(der: bytes) -> dict:
    _, cs, ce = tlv(der, 0)
    tbs = children(der, *tlv(der, cs)[1:])
    if tbs[0][0] == 0xA0:                 # the explicit version
        tbs = tbs[1:]
    # serial, signature algorithm, issuer, validity, subject, subjectPublicKeyInfo
    subj = tbs[4]
    dn = der[subj[1]:subj[3]]
    spki = children(der, tbs[5][2], tbs[5][3])
    alg = children(der, spki[0][2], spki[0][3])
    oid = der[alg[0][2]:alg[0][3]]
    bits = der[spki[1][2] + 1:spki[1][3]]   # the BIT STRING, past its unused-bits byte
    if oid == OID_RSA:
        n_e = children(bits, *tlv(bits, 0)[1:])
        n = bits[n_e[0][2]:n_e[0][3]].lstrip(b"\0")
        e = bits[n_e[1][2]:n_e[1][3]].lstrip(b"\0")
        return {"dn": dn, "kind": "RSA", "n": n, "e": e}
    if oid == OID_EC:
        curve = der[alg[1][2]:alg[1][3]]
        return {"dn": dn, "kind": "EC", "curve": CURVES[curve], "q": bits}
    raise ValueError("neither an RSA nor an EC key")


def c_bytes(name: str, b: bytes) -> str:
    rows = [", ".join(f"0x{x:02X}" for x in b[i:i + 12]) for i in range(0, len(b), 12)]
    return f"static const unsigned char {name}[] = {{\n    " + ",\n    ".join(rows) + "\n};\n"


def generate(text: str) -> str:
    certs = re.findall(r"(?:# ([^\n]*)\n)?-----BEGIN CERTIFICATE-----(.*?)-----END CERTIFICATE-----", text, re.S)
    out = ["/* The certificate authorities plansync.c trusts: generated from roots.pem by",
           " * ghostldr/tools/gen_trust_anchors.py; edit roots.pem, not this file. */",
           "#pragma GCC visibility push(hidden)", '#include "bearssl.h"', ""]
    entries = []
    for i, (label, body) in enumerate(certs):
        a = anchor(base64.b64decode("".join(body.split())))
        out.append(f"/* {label or f'certificate {i}'} */")
        out.append(c_bytes(f"DN{i}", a["dn"]))
        if a["kind"] == "RSA":
            out.append(c_bytes(f"RSA_N{i}", a["n"]))
            out.append(c_bytes(f"RSA_E{i}", a["e"]))
            key = (f"{{BR_KEYTYPE_RSA, {{.rsa = {{(unsigned char *)RSA_N{i}, sizeof RSA_N{i}, "
                   f"(unsigned char *)RSA_E{i}, sizeof RSA_E{i}}}}}}}")
        else:
            out.append(c_bytes(f"EC_Q{i}", a["q"]))
            key = f"{{BR_KEYTYPE_EC, {{.ec = {{{a['curve']}, (unsigned char *)EC_Q{i}, sizeof EC_Q{i}}}}}}}"
        entries.append(f"    {{{{(unsigned char *)DN{i}, sizeof DN{i}}}, BR_X509_TA_CA, {key}}},")
    out.append("const br_x509_trust_anchor PS_TAs[] = {")
    out += entries
    out.append("};")
    out.append(f"const unsigned PS_TAs_NUM = {len(entries)};")
    return "\n".join(out) + "\n"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--check", action="store_true", help="fail if plansync_ta.c doesn't match roots.pem")
    args = ap.parse_args()
    text = generate(SRC.read_text())
    if args.check:
        if not OUT.exists() or OUT.read_text() != text:
            print(f"{OUT.relative_to(ROOT)} is stale: run ghostldr/tools/gen_trust_anchors.py", file=sys.stderr)
            return 1
        return 0
    OUT.write_text(text)
    print(f"{OUT.relative_to(ROOT)}: {text.count('BR_X509_TA_CA')} certificate authorities")
    return 0


if __name__ == "__main__":
    sys.exit(main())
