# Design mocks

Browser mock-ups of what the board's panels show, used to decide the readouts
before any of it was written in C. Open them directly; they are plain HTML with
no build step and no dependencies.

| File | What it was for |
| --- | --- |
| [screens.html](screens.html) | Every panel as it looked before the readouts — the baseline the rest argued against. |
| [screens2.html](screens2.html) | The first proposal against that baseline. |
| [themes.html](themes.html) | Palettes, when a theme was still only a palette. |
| [layouts.html](layouts.html) | The ten readouts, which is what shipped. |

These are **not** the firmware. They drift the moment a layout changes, and
nothing checks them — `firmware/src/main.cpp` is the truth about what a board
draws. They are kept because the reasoning in them is hard to reconstruct from
the C, and because the next theme is easier to try here than on a panel.

They live outside `docs/` on purpose: the Pages workflow publishes that
directory verbatim, so anything in it is public.

## The README's pictures

`make-readme-images.py` builds `docs/img/readouts.svg` and `docs/img/boards.svg`
from `layouts.html`, by parsing it rather than redrawing it — so the pictures in
the README cannot quietly stop matching the mock they came from. Change a
layout here, re-run it from the repo root, commit both.

```
python design/make-readme-images.py
```

SVG rather than screenshots, because GitHub strips inline `<svg>` from Markdown
but serves committed files fine, and a file that regenerates from its source
beats a PNG nobody can edit.
