# ghostty-web source build

Source: https://github.com/coder/ghostty-web
Revision: `640db4617c69a73ccebce437cebc598c7f73cdb7` (0.4.0)
Ghostty submodule: `5714ed07a1012573261b7b7e3ed2add9c1504496`
Build tools: Zig 0.15.2, Bun 1.3.2.

Run `yarn build:ghostty` from `html/` to rebuild the WASM, JavaScript and
TypeScript declarations from Git. The upstream WASM API patch is applied by
its build script. No prebuilt npm terminal artifacts are used.

The upstream Vite library build embeds the compiled WASM in `ghostty-web.js`.
`ghostty-vt.wasm` is also retained as a standalone build artifact. The tiny
`__vite-browser-external-*.js` module is Vite's browser stub for Node's filesystem.
Webpack combines it into the application so no extra runtime requests are needed.

After rebuilding, run `yarn install` to refresh the local package cache and lockfile,
then `yarn build` to regenerate `src/html.h`.

`LICENSE` covers ghostty-web. `GHOSTTY-LICENSE` covers the Ghostty WASM source.
