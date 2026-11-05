## Prerequisites

Use Node.js 18+ and the pinned Yarn 3.6.3 (`corepack enable`), then run
`yarn install` in this directory.

## Terminal assets

The terminal uses [ghostty-web](https://github.com/coder/ghostty-web), built from
Git at the revision recorded in `vendor/ghostty-web/README.md`. Both its
JavaScript and Ghostty WebAssembly are built from source. The generated assets
are checked in so normal frontend builds need only Node and Yarn.

To rebuild from Git, install Zig 0.15.2 and Bun 1.3.2, then run:

```sh
yarn build:ghostty
yarn install
```

JetBrains Mono **2.304**, the latest official release checked on 2026-11-04,
is bundled in regular, bold, italic and bold italic. `yarn build:fonts` downloads
and verifies the official release archive to restore the fonts.

WASM and fonts are embedded in the generated page, with their licenses. The
page needs no CDN or additional asset routes, including behind a base path.

## Development

1. Start ttyd: `ttyd -W bash` or `ttyd -W zsh`.
2. Start the dev server: `yarn start`.

Dracula is the default terminal theme for both Bash and Zsh. This controls
terminal colors; existing shell prompts and shell configuration still apply.
Override individual colors with ttyd client options, for example:

```sh
ttyd -W -t 'theme={"background":"#1e1e2e"}' zsh
```

Ghostty uses its built-in canvas renderer and Unicode handling. Legacy
`rendererType` and `unicodeVersion` options are accepted for compatibility.
Sixel images and OSC 52 clipboard access are not supported by this renderer.
ZMODEM and trzsz file transfers remain available.


The browser defaults to 12px JetBrains Mono with `letterSpacing=-0.5` and
`lineHeight=1.2`. Tune column spacing and row spacing independently:

```sh
ttyd -W -t fontSize=12 -t letterSpacing=-0.5 -t lineHeight=1.2 bash
```

`fontWidth` scales the glyphs horizontally: `1` is their natural shape, `0.9`
is 10% narrower, and `1.1` is 10% wider. It preserves character height and adjusts
cell width to keep selection and cursor alignment correct. For example:

```sh
ttyd -W -t fontSize=12 -t fontWidth=0.9 -t letterSpacing=0 -t lineHeight=1.2 bash
```

A more negative `letterSpacing` tightens columns; a larger `lineHeight` adds
space between rows. These options also work in the browser URL, for example
`?fontWidth=0.9&letterSpacing=0&lineHeight=1.2`.

## Publish

Run `yarn check` and `yarn build`. This generates `dist/inline.html` and embeds
the compressed page in `../src/html.h`. Rebuild the C executable afterwards.
