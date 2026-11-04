#!/usr/bin/env python3
"""hello_cube PAPP:  python examples/papp/hello_cube/build.py -> build/e3d_hello_cube/"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import papp_common  # noqa: E402

if __name__ == "__main__":
    papp_common.build("e3d_hello_cube", HERE, ["main.c"])
