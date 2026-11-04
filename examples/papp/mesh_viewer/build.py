#!/usr/bin/env python3
"""mesh_viewer PAPP:  python examples/papp/mesh_viewer/build.py -> build/e3d_mesh_viewer/

The built-in model is examples/assets/knot.stl, turned into a C header by
tools/stl2e3d.py as part of the build (build/e3d_mesh_viewer/gen/knot.h).
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import papp_common  # noqa: E402

if __name__ == "__main__":
    gen = os.path.join(papp_common.ROOT, "build", "e3d_mesh_viewer", "gen")
    os.makedirs(gen, exist_ok=True)
    subprocess.check_call([sys.executable, os.path.join(papp_common.ROOT, "tools", "stl2e3d.py"),
                           os.path.join(papp_common.ROOT, "examples", "assets", "knot.stl"),
                           "--header", os.path.join(gen, "knot.h"), "--size", "200", "--color", "E08030"])
    papp_common.build("e3d_mesh_viewer", HERE, ["main.c"], includes=[gen])
