#!/usr/bin/env python3
"""Check that the AES key in an environment variable opens an encrypted .espdl model, without printing the key.

  CARMIND_MODEL_KEY_EV=... python3 tools/check_model_key.py ml/onboard/onboard.espdl CARMIND_MODEL_KEY_EV

ESP-DL encrypts the model body with AES-128-CTR (fixed counter block 0x10..0x1F, fbs_loader.cpp). With the right key the
body is a FlatBuffer whose root offset and root vtable point inside it; a wrong key gives random bytes that almost never
pass. Exit 0: the key opens the file, or the file is not encrypted. Exit 1: wrong key or bad file. Exit 2: cannot check
here (the Python package cryptography is missing).
"""
import os
import struct
import sys

NONCE = bytes(range(0x10, 0x20))


def plausible_flatbuffer(body):
    if len(body) < 16:
        return False
    root = struct.unpack_from("<I", body, 0)[0]
    if root % 4 or not 8 <= root <= len(body) - 4:
        return False
    vtable = root - struct.unpack_from("<i", body, root)[0]
    if not 0 <= vtable <= len(body) - 4:
        return False
    vt_size, table_size = struct.unpack_from("<HH", body, vtable)
    return vt_size >= 4 and vt_size % 2 == 0 and vt_size < 1024 and 4 <= table_size < 65536


def main(path, env_name):
    data = open(path, "rb").read()
    magic, mode, size = data[:4], *struct.unpack_from("<II", data, 4)
    if magic != b"EDL2":
        print(f"{path}: not an EDL2 file")
        return 1
    if mode == 0:
        print(f"{path}: not encrypted, no key needed")
        return 0
    key_hex = os.environ.get(env_name, "").strip()
    if len(key_hex) != 32:
        print(f"{env_name}: not set or not 32 hex characters")
        return 1
    try:
        from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
    except ImportError:
        print("check_model_key.py: the Python package cryptography is missing, key not checked")
        return 2
    dec = Cipher(algorithms.AES(bytes.fromhex(key_hex)), modes.CTR(NONCE)).decryptor()
    body = dec.update(data[16:16 + size]) + dec.finalize()
    if not plausible_flatbuffer(body):
        print(f"{env_name} does not open {os.path.basename(path)}")
        return 1
    print(f"{env_name} opens {os.path.basename(path)}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1], sys.argv[2]))
