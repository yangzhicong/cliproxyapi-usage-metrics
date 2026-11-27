#!/usr/bin/env python3
import base64
import os

here = os.path.dirname(os.path.abspath(__file__))
ui_path = os.path.join(here, "ui.html")
out_path = os.path.join(here, "ui_assets.h")

with open(ui_path, "rb") as f:
    b64 = base64.b64encode(f.read()).decode("ascii")

with open(out_path, "w", encoding="utf-8") as f:
    f.write("/* Auto-generated from ui.html */\n")
    f.write("#ifndef UI_ASSETS_H\n#define UI_ASSETS_H\n\n")
    f.write("static const char EMBEDDED_INDEX_HTML_B64[] =\n")
    for i in range(0, len(b64), 100):
        f.write(f'    "{b64[i:i+100]}"\n')
    f.write(";\n\n#endif\n")

print(f"Generated {out_path} ({len(b64)} base64 bytes)")
