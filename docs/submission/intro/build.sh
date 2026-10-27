#!/usr/bin/env bash
# Build the 작품소개서: the annotated model photo, then fill the template and export with Word.
#   bash build.sh            (Windows PC with Word and the 나눔명조 font)
set -euo pipefail
cd "$(dirname "$0")"
PYTHONUTF8=1 python -I photo.py >/dev/null
PYTHONUTF8=1 python -I build_intro.py
rm -f out/page-*.png
PYTHONUTF8=1 python -I -c "
import fitz, glob
pdf = [p for p in glob.glob('out/*작품소개서.pdf')][0]
d = fitz.open(pdf)
for i, p in enumerate(d):
    p.get_pixmap(matrix=fitz.Matrix(1.6, 1.6)).save(f'out/page-{i+1}.png')
print(d.page_count, 'pages ->', pdf)
"
