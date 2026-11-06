# Lucide icons

The editor uses outlines from [Lucide](https://lucide.dev), revision
`ee8e10d746a10907205366cfd984922f0c58939a` (2026-11-05).
The `lucide.paths` file is a compact, offline representation of selected SVG
outlines (`P` = SVG path, `R` = rounded rect, `C` = circle), in Lucide's 24 px
coordinate system. The generated `src/app/ui_lucide_data.h` is built from those
paths. The original Lucide ISC/Feather MIT terms are in `LUCIDE-LICENSE`.

Regenerate only when the curated outlines change:

```sh
python3 -m pip install fonttools
python3 tools/generate_lucide_icons.py
python3 tools/generate_lucide_icons.py --check
```

This is a developer-only operation; the editor requires no icon font,
SVG parser, extra GPU texture, asset file, or network connection at runtime.
