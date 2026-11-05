# ttyd - Share your terminal over the web

ttyd shares a terminal in your browser. This version uses Ghostty's terminal
emulator compiled to WebAssembly, with JetBrains Mono and Dracula colors.

# Features

- [ghostty-web](https://github.com/coder/ghostty-web) terminal with a canvas renderer and Ghostty's VT parser, built from Git.
- Bundled JetBrains Mono 2.304, the latest official release, in regular, bold, italic and bold italic.
- Dracula terminal colors for Bash and Zsh, with configurable fonts, cursor and colors.
- Self-contained embedded frontend: JavaScript, WebAssembly and fonts work without a CDN.
- Responsive terminal sizing, Unicode text, selection, clipboard copy/paste and clickable links.
- ZMODEM and trzsz file transfers, including drag and drop with trzsz.
- libuv and WebSocket backend with TLS, basic authentication and reverse proxy support.
- Run Bash, Zsh or another command with arguments; enable browser input with `-W`.

# Installation

## Compile on Linux

Install CMake, Git, a C compiler and development libraries. libwebsockets must
include support for the libuv event loop.

On Arch Linux:

```sh
sudo pacman -S --needed base-devel cmake git libuv libwebsockets json-c zlib openssl
```

On Alpine Linux:

```sh
sudo apk add build-base cmake git libuv-dev libwebsockets-dev \
  libwebsockets-evlib_uv json-c-dev zlib-dev openssl-dev
```

Clone and compile this repository, then install the executable and manual page:

```sh
git clone https://github.com/pbtrung/ttyd.git
cd ttyd
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
sudo cmake --install build
```

The generated frontend is checked in as `src/html.h`, so compiling the C server
does not require Node.js, Bun or Zig.

To rebuild the frontend, use Node.js 18+ and Yarn 3.6.3:

```sh
corepack enable
corepack prepare yarn@3.6.3 --activate
cd html
yarn install
yarn check
yarn build
cd ..
cmake --build build --parallel
sudo cmake --install build
```

To rebuild Ghostty's `.wasm` and `.js` from the pinned Git sources, install
Zig 0.15.2 and Bun 1.3.2, then run `yarn build:ghostty` and `yarn install` in
`html/` before `yarn build`. `yarn build:fonts` restores the bundled fonts from
the verified JetBrains Mono release archive. See [frontend build details](html/README.md).

Start an interactive session and open `http://localhost:7681`:

```sh
ttyd -W bash
# Or:
ttyd -W zsh
```

# Usage

## Command-line Options

```
USAGE:
    ttyd [options] <command> [<arguments...>]

OPTIONS:
    -p, --port              Port to listen (default: 7681, use `0` for random port)
    -i, --interface         Network interface to bind (eg: eth0), or UNIX domain socket path (eg: /var/run/ttyd.sock)
    -U, --socket-owner      User owner of the UNIX domain socket file, when enabled (eg: user:group)
    -c, --credential        Credential for basic authentication (format: username:password)
    -H, --auth-header       HTTP Header name for auth proxy, this will configure ttyd to let a HTTP reverse proxy handle authentication
    -u, --uid               User id to run with
    -g, --gid               Group id to run with
    -s, --signal            Signal to send to the command when exit it (default: 1, SIGHUP)
    -w, --cwd               Working directory to be set for the child program
    -a, --url-arg           Allow client to send command line arguments in URL (eg: http://localhost:7681?arg=foo&arg=bar)
    -W, --writable          Allow clients to write to the TTY (readonly by default)
    -t, --client-option     Send option to client (format: key=value), repeat to add more options
    -T, --terminal-type     Terminal type to report, default: xterm-256color
    -O, --check-origin      Do not allow websocket connection from different origin
    -m, --max-clients       Maximum clients to support (default: 0, no limit)
    -o, --once              Accept only one client and exit on disconnection
    -q, --exit-no-conn      Exit on all clients disconnection
    -B, --browser           Open terminal with the default system browser
    -I, --index             Custom index.html path
    -b, --base-path         Expected base path for requests coming from a reverse proxy (eg: /mounted/here, max length: 128)
    -P, --ping-interval     Websocket ping interval(sec) (default: 5)
    -6, --ipv6              Enable IPv6 support
    -S, --ssl               Enable SSL
    -C, --ssl-cert          SSL certificate file path
    -K, --ssl-key           SSL key file path
    -A, --ssl-ca            SSL CA file path for client certificate verification
    -d, --debug             Set log level (default: 7)
    -v, --version           Print the version and exit
    -h, --help              Print this text and exit
```

Read the example usage on the [wiki](https://github.com/tsl0922/ttyd/wiki/Example-Usage).

## Browser Support

Modern browsers with WebAssembly, Canvas, WebSocket, and Font Loading API support.
