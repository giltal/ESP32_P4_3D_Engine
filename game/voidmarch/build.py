#!/usr/bin/env python3
"""Voidmarch PAPP:  python game/voidmarch/build.py -> build/voidmarch/voidmarch.papp"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(HERE)), "examples", "papp"))
import papp_common  # noqa: E402

if __name__ == "__main__":
    print(papp_common.build("voidmarch", HERE, ["main.c"]))
