Import("env")

src = "data/index.html"
dst = "include/index_html.h"

with open(src, "rb") as f:
    data = f.read()

out = []
out.append("#pragma once\n")
out.append(f"// Auto-generated from {src} by scripts/gen_html.py — do not edit\n")
out.append(f"static const unsigned int INDEX_HTML_LEN = {len(data)};\n")
out.append("static const char INDEX_HTML[] = {\n")

for i, b in enumerate(data):
    if i % 16 == 0:
        out.append("    ")
    out.append(f"0x{b:02x},")
    if (i + 1) % 16 == 0:
        out.append("\n")

out.append("\n    0x00\n};\n")

with open(dst, "w") as f:
    f.writelines(out)
