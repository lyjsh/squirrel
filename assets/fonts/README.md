# Project fonts

Put redistributable font files in this directory to make the app use fonts shipped with the project.

Recommended filenames:

- `ui-regular.ttf` or `ui-regular.ttc` for the normal UI Latin font.
- `ui-bold.ttf` or `ui-bold.ttc` for title text.
- `cjk-regular.ttf` or `cjk-regular.ttc` for Chinese/Japanese/Korean glyphs.
- `cjk-bold.ttf` or `cjk-bold.ttc` for bold CJK title glyphs.
- `code-latin.ttf` or `code-latin.ttc` for monospace Latin code text.
- `code-cjk.ttf` or `code-cjk.ttc` for CJK code text.

For local testing, the loader also recognizes the old Windows filenames such as
`segoeui.ttf`, `segoeuib.ttf`, `msyh.ttc`, `msyhbd.ttc`, `consola.ttf`, and `cour.ttf`.

If none of the names above exist, the loader will still try the first `.ttf`, `.ttc`, or `.otf`
file found in this directory.

Only commit font files that are licensed for redistribution.

Current local bundle:

- `ui-regular.otf` and `cjk-regular.otf`: copied from local `Alibaba-PuHuiTi-Regular.otf`.
- `code-latin.ttf`: copied from local JetBrains Runtime `JetBrainsMono-Regular.ttf`.
