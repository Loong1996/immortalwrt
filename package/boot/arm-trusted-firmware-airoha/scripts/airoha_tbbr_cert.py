#!/usr/bin/env python3
#
# Build the TBBR certificates that an Airoha chip with a fused root-of-trust
# key checks at boot.
#
# The BootROM verifies BL2: it takes the public key from the self-signed
# "Trusted Boot FW Certificate", compares its hash against the efuse, checks
# the signature and then the BL2 hash in extension 1.3.6.1.4.1.4128.2100.201.
#
# BL2 is built with TRUSTED_BOARD_BOOT.  The closed-source part turns
# authentication off at runtime on chips without a key, so the same BL2 runs
# anywhere; on a fused chip it walks the whole chain of trust before running
# BL31 and U-Boot, and a FIP without the certificates fails with -ENOENT.
#
# The layout matches what TF-A's cert_create emits (and what the vendor
# bootloaders carry): X.509 v3, RSA-PSS / SHA-512 / MGF1-SHA-512, salt 32,
# subject = issuer, SKI/AKI = SHA-1 of the public key, basic constraints and
# the critical TBBR extensions.  cert_create signs every level with its own
# key; here all of them use the root-of-trust key, which the chain of trust
# allows since each level only checks against the key the level above names.
#
# Standard library only, so it runs on any build host without OpenSSL.
#
# Usage:
#   airoha_tbbr_cert.py tb-fw <rot-key.pem> <bl2.bin> <tb-fw-cert.der>
#   airoha_tbbr_cert.py fip <rot-key.pem> <bl31> <bl33> <out-dir>
#
# "fip" writes trusted-key.crt, soc-fw-key.crt, nt-fw-key.crt, soc-fw.crt and
# nt-fw.crt for fiptool.  <bl31> and <bl33> are the files that go into the
# FIP (the LZMA images): BL2 hashes an image as it reads it from the FIP.

import base64
import datetime
import hashlib
import os
import re
import sys

OID_RSA = '1.2.840.113549.1.1.1'
OID_PSS = '1.2.840.113549.1.1.10'
OID_MGF1 = '1.2.840.113549.1.1.8'
OID_SHA512 = '2.16.840.1.101.3.4.2.3'
OID_CN = '2.5.4.3'
OID_SKI = '2.5.29.14'
OID_AKI = '2.5.29.35'
OID_BC = '2.5.29.19'
TBBR = '1.3.6.1.4.1.4128.2100.'
OID_TRUSTED_NV_CTR = TBBR + '1'
OID_NON_TRUSTED_NV_CTR = TBBR + '2'
OID_TB_FW_HASH = TBBR + '201'
OID_TB_FW_CONFIG_HASH = TBBR + '202'
OID_HW_CONFIG_HASH = TBBR + '203'
OID_TRUSTED_WORLD_PK = TBBR + '302'
OID_NON_TRUSTED_WORLD_PK = TBBR + '303'
OID_SOC_FW_CONTENT_CERT_PK = TBBR + '501'
OID_SOC_AP_FW_HASH = TBBR + '603'
OID_SOC_FW_CONFIG_HASH = TBBR + '604'
OID_NT_FW_CONTENT_CERT_PK = TBBR + '1101'
OID_NT_WORLD_BL_HASH = TBBR + '1201'
OID_NT_FW_CONFIG_HASH = TBBR + '1202'

SALT_LEN = 32
VALID_YEARS = 20


# ---- DER encoding --------------------------------------------------------

def tlv(tag, body):
    n = len(body)
    if n < 0x80:
        ln = bytes([n])
    else:
        b = n.to_bytes((n.bit_length() + 7) // 8, 'big')
        ln = bytes([0x80 | len(b)]) + b
    return bytes([tag]) + ln + body


def seq(*items):
    return tlv(0x30, b''.join(items))


def integer(v):
    b = v.to_bytes(max(1, (v.bit_length() + 8) // 8), 'big')
    return tlv(0x02, b)


def oid(dotted):
    parts = [int(x) for x in dotted.split('.')]
    body = bytes([parts[0] * 40 + parts[1]])
    for p in parts[2:]:
        chunk = [p & 0x7f]
        p >>= 7
        while p:
            chunk.insert(0, 0x80 | (p & 0x7f))
            p >>= 7
        body += bytes(chunk)
    return tlv(0x06, body)


def octets(b):
    return tlv(0x04, b)


def bits(b):
    return tlv(0x03, b'\x00' + b)


def ctx(n, body):
    return tlv(0xa0 | n, body)


def utctime(t):
    return tlv(0x17, t.strftime('%y%m%d%H%M%SZ').encode())


def name(cn):
    return seq(tlv(0x31, seq(oid(OID_CN), tlv(0x0c, cn.encode()))))


def ext(o, value, critical=False):
    return seq(oid(o), *([tlv(0x01, b'\xff')] if critical else []),
               octets(value))


def digest_info(h):
    return seq(seq(oid(OID_SHA512), tlv(0x05, b'')), octets(h))


# ---- DER decoding (just enough for RSA private keys) ---------------------

def read_tlv(d, i):
    tag = d[i]
    ln = d[i + 1]
    i += 2
    if ln & 0x80:
        k = ln & 0x7f
        ln = int.from_bytes(d[i:i + k], 'big')
        i += k
    return tag, d[i:i + ln], i + ln


def read_seq(d):
    tag, body, _ = read_tlv(d, 0)
    if tag != 0x30:
        raise ValueError('not a SEQUENCE')
    out, i = [], 0
    while i < len(body):
        t, v, i = read_tlv(body, i)
        out.append((t, v))
    return out


def load_rsa_key(path):
    text = open(path).read()
    m = re.search(r'-----BEGIN (RSA )?PRIVATE KEY-----(.*?)-----END', text, re.S)
    if not m:
        raise ValueError('%s: no PEM private key' % path)
    der = base64.b64decode(''.join(m.group(2).split()))
    if not m.group(1):
        # PKCS#8: version, algorithm, OCTET STRING { RSAPrivateKey }
        der = read_seq(der)[2][1]
    f = [int.from_bytes(v, 'big') for t, v in read_seq(der)]
    # version, n, e, d, p, q, dp, dq, qinv
    return dict(zip(('n', 'e', 'd', 'p', 'q', 'dp', 'dq', 'qinv'), f[1:9]))


# ---- RSASSA-PSS ----------------------------------------------------------

def mgf1(seed, length):
    out, c = b'', 0
    while len(out) < length:
        out += hashlib.sha512(seed + c.to_bytes(4, 'big')).digest()
        c += 1
    return out[:length]


def pss_sign(key, msg):
    n = key['n']
    mod_bits = n.bit_length()
    em_bits = mod_bits - 1
    em_len = (em_bits + 7) // 8
    m_hash = hashlib.sha512(msg).digest()
    # Deterministic salt: the build stays reproducible, and PSS does not need
    # the salt to be secret.
    salt = hashlib.sha512(b'airoha-tb-fw-salt' + m_hash).digest()[:SALT_LEN]
    h = hashlib.sha512(b'\x00' * 8 + m_hash + salt).digest()
    ps = b'\x00' * (em_len - SALT_LEN - len(h) - 2)
    db = ps + b'\x01' + salt
    masked = bytes(a ^ b for a, b in zip(db, mgf1(h, len(db))))
    masked = bytes([masked[0] & (0xff >> (8 * em_len - em_bits))]) + masked[1:]
    em = int.from_bytes(masked + h + b'\xbc', 'big')
    # CRT
    m1 = pow(em, key['dp'], key['p'])
    m2 = pow(em, key['dq'], key['q'])
    s = m2 + key['q'] * ((key['qinv'] * (m1 - m2)) % key['p'])
    if pow(s, key['e'], n) != em:
        raise ValueError('RSA self-check failed')
    return s.to_bytes((mod_bits + 7) // 8, 'big')


# ---- the certificates ----------------------------------------------------

def spki(key):
    pub = seq(integer(key['n']), integer(key['e']))
    return pub, seq(seq(oid(OID_RSA), tlv(0x05, b'')), bits(pub))


def validity():
    epoch = int(os.environ.get('SOURCE_DATE_EPOCH', '0')) or None
    start = (datetime.datetime.fromtimestamp(epoch, datetime.timezone.utc)
             if epoch else datetime.datetime.now(datetime.timezone.utc))
    start = start.replace(microsecond=0)
    end = start.replace(year=start.year + VALID_YEARS)
    return seq(utctime(start), utctime(end))


def cert(key, cn, tbbr_exts):
    """Self-signed certificate carrying the given critical TBBR extensions."""
    pub, pk = spki(key)
    keyid = hashlib.sha1(pub).digest()

    pss = seq(oid(OID_PSS), seq(
        ctx(0, seq(oid(OID_SHA512))),
        ctx(1, seq(oid(OID_MGF1), seq(oid(OID_SHA512)))),
        ctx(2, integer(SALT_LEN))))

    exts = [ext(OID_SKI, octets(keyid)),
            ext(OID_AKI, seq(tlv(0x80, keyid))),
            ext(OID_BC, seq())]
    exts += [ext(o, v, critical=True) for o, v in tbbr_exts]
    body = seq(*exts)

    # Serial derived from the content: reproducible, and distinct per image.
    serial = int.from_bytes(hashlib.sha512(cn.encode() + body).digest()[:8],
                            'big')

    tbs = seq(ctx(0, integer(2)), integer(serial), pss, name(cn),
              validity(), name(cn), pk, ctx(3, body))

    return seq(tbs, pss, bits(pss_sign(key, tbs)))


ZERO_HASH = digest_info(b'\x00' * 64)


def sha(data):
    return digest_info(hashlib.sha512(data).digest())


def tb_fw(key, bl2):
    return cert(key, 'Trusted Boot FW Certificate', [
        (OID_TRUSTED_NV_CTR, integer(0)),
        (OID_TB_FW_HASH, sha(bl2)),
        (OID_TB_FW_CONFIG_HASH, ZERO_HASH),
        (OID_HW_CONFIG_HASH, ZERO_HASH)])


def fip(key, bl31, bl33):
    pk = spki(key)[1]
    return {
        'trusted-key.crt': cert(key, 'Trusted Key Certificate', [
            (OID_TRUSTED_NV_CTR, integer(0)),
            (OID_TRUSTED_WORLD_PK, pk),
            (OID_NON_TRUSTED_WORLD_PK, pk)]),
        'soc-fw-key.crt': cert(key, 'SoC Firmware Key Certificate', [
            (OID_TRUSTED_NV_CTR, integer(0)),
            (OID_SOC_FW_CONTENT_CERT_PK, pk)]),
        'nt-fw-key.crt': cert(key, 'Non-Trusted Firmware Key Certificate', [
            (OID_NON_TRUSTED_NV_CTR, integer(0)),
            (OID_NT_FW_CONTENT_CERT_PK, pk)]),
        'soc-fw.crt': cert(key, 'SoC Firmware Content Certificate', [
            (OID_TRUSTED_NV_CTR, integer(0)),
            (OID_SOC_AP_FW_HASH, sha(bl31)),
            (OID_SOC_FW_CONFIG_HASH, ZERO_HASH)]),
        'nt-fw.crt': cert(key, 'Non-Trusted Firmware Content Certificate', [
            (OID_NON_TRUSTED_NV_CTR, integer(0)),
            (OID_NT_WORLD_BL_HASH, sha(bl33)),
            (OID_NT_FW_CONFIG_HASH, ZERO_HASH)]),
    }


def read(path):
    with open(path, 'rb') as f:
        return f.read()


def write(path, data):
    with open(path, 'wb') as f:
        f.write(data)


def main():
    a = sys.argv[1:]
    if len(a) == 4 and a[0] == 'tb-fw':
        write(a[3], tb_fw(load_rsa_key(a[1]), read(a[2])))
    elif len(a) == 5 and a[0] == 'fip':
        os.makedirs(a[4], exist_ok=True)
        for n, der in fip(load_rsa_key(a[1]), read(a[2]), read(a[3])).items():
            write(os.path.join(a[4], n), der)
    else:
        sys.exit('usage: %s tb-fw <rot-key.pem> <bl2.bin> <tb-fw-cert.der>\n'
                 '       %s fip <rot-key.pem> <bl31> <bl33> <out-dir>'
                 % (sys.argv[0], sys.argv[0]))


if __name__ == '__main__':
    main()
