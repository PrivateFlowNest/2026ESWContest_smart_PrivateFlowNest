#!/usr/bin/env bash
# Build the report, export the PDF with PowerPoint (fonts embedded) and render page images for checking.
#   bash build.sh            (Windows PC with PowerPoint; PPTX_SKILL_DIR optional, for theme colors)
set -euo pipefail
cd "$(dirname "$0")"
NODE_PATH="$(cygpath -w "$PWD/node_modules" 2>/dev/null || echo "$PWD/node_modules")" node build_report.js
deck="out/2026ESWContest_스마트가전_PrivateFlowNest_개발완료보고서.pptx"
powershell -ExecutionPolicy Bypass -File render.ps1 "$deck" >/dev/null
rm -f out/slide-*.png
PYTHONUTF8=1 python -I -c "
import fitz, glob
pdf = glob.glob('out/*개발완료보고서.pdf')[0]
d = fitz.open(pdf)
for i, p in enumerate(d):
    p.get_pixmap(matrix=fitz.Matrix(1.6, 1.6)).save(f'out/slide-{i+1:02d}.png')
print(d.page_count, 'pages ->', pdf)
"
