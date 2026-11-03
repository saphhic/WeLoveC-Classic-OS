<!-- Written by [@saphhic](https://github.com/saphhic) -->
<!-- Date: October 31st 2026 -->
<!-- Description: Records the browser's current implementation and limits. -->

# WeLoveC Classic browser status

WeLoveC Classic includes an early text browser for the desktop, using HTTP and
HTTPS over the userland network stack. It is a compiled prototype that fetches
and displays pages. It is not a general-purpose browser and is not ready for
current sites that depend on CSS, JavaScript, or multimedia.

## Implemented components

- **Desktop application:** `userland/bin/browser/main.c` builds as
  `/userspace/bin/browser/browser.elf`. The desktop discovers it with the
  other programs and runs it in a window with its own keyboard input. It
  supports address editing, reload, scrolling, a history of up to 16 pages,
  and keyboard navigation between links.
- **URLs:** `userland/lib/web/url.c` parses HTTP and HTTPS URLs, ASCII DNS
  hostnames, IPv6 literals, and ports. It drops fragments and resolves
  absolute, relative, and path references. It does not implement IDNA, full
  canonicalization, or complete percent encoding.
- **HTTP client:** `userland/lib/web/http.c` sends GET requests, limits headers
  to 16 KiB, and accepts bodies delimited by `Content-Length` or
  `Transfer-Encoding: chunked`. It follows up to five redirects, supports
  relative references, and rejects redirects from HTTPS to HTTP. It sends no
  cookies or credentials and marks HTTP pages as unencrypted. It does not
  accept bodies delimited only by connection close or compressed content such
  as gzip.
- **HTTPS:** The Mbed TLS adapter requires TLS 1.2 or newer and checks the
  certificate and hostname against the system CA bundle. Certificate errors
  are shown to the user; there is no silent HTTP retry.
- **HTML:** `third_party/netsurf/netsurf-all-3.11.tar.gz` provides the NetSurf
  Hubbub parser and tokenizer. The browser callback consumes tags, attributes,
  and decoded text; detects the document charset; skips `script`, `style`, and
  `head` content; inserts block breaks; preserves whitespace in `pre`; and
  shows image alternative text. Hubbub's tree builder and DOM are not enabled.
- **Links:** The browser collects up to 64 HTTP(S) destinations and resolves
  relative paths against the first valid `<base href>`, or the final page URL
  when there is no base URL. It prefixes links with numbers in the text. `[` and
  `]` select links, and Enter opens the selected link. Other URL schemes are
  not activated.
- **Plain text:** Non-HTML responses are displayed directly. The application
  limits each body to 256 KiB.
- **Networking:** It uses DNS A/AAAA and IPv4/IPv6 TCP exposed to userland.
  Requests are sequential, and the browser does not fetch secondary resources.
  Browser errors identify DNS failures, missing routes, missing kernel entropy,
  refused ports, TCP handshake timeouts, send failures, and receive failures.
  A timeout means that no response to the SYN arrived; by itself, it does not
  identify whether the server, port, or a firewall caused the silence.

TCP also needs secure entropy for sequence numbers and local ports. The kernel
gets it from the UEFI RNG, RDSEED/RDRAND, or a VirtIO RNG device. QEMU can add
`-device virtio-rng-pci` to its existing configuration; the virtual machine
must then be restarted. The VirtIO RNG driver is part of the kernel. The system
does not replace this source with predictable values because the same
generator protects TLS and other security functions.

HTTPS first needs a TCP connection to port 443 and then a TLS handshake. A site
serving only HTML does not determine which protocol it uses: HTML can be served
over HTTP or HTTPS. If the server offers only HTTP, open `http://host/`; an
`https://` URL does not switch to port 80 or downgrade to HTTP automatically.

## Current limits

1. **There is no complete web engine.** Hubbub tokenizes and parses HTML, but
   the browser does not build a DOM or apply HTML tree-repair rules. It also has
   no CSS layout, JavaScript, scalable fonts, image loading, audio/video,
   forms, or form submission. Images can show only their `alt` text.
2. **The interface is basic.** There are no tabs, bookmarks, downloads,
   cookies, per-site permissions, settings, or web storage. Links are limited
   to HTTP(S), and history keeps at most 16 URLs.
3. **Isolation is insufficient for hostile content.** The browser runs as a
   separate process from the desktop, but it has no sandbox or reduced
   permissions. It retains the normal capabilities of its user, including
   network access.
4. **The transport needs more work.** TCP currently uses stop-and-wait and
   processes data in order. Congestion control, window scaling, out-of-order
   segments, and complete FIN handling are missing. Interoperability with
   packet loss and different servers also needs validation. The receive syscall
   now polls the network once without blocking before draining received bytes;
   without that poll, the browser could not see TCP responses even when ICMP
   worked.
5. **Web support is deliberately limited.** There is no IDNA, full URL
   canonicalization, HTTP compression, close-delimited responses, or downloads
   larger than 256 KiB in this application. The HTML parser is not a complete
   browser or layout engine.

## Next steps

1. Exercise the HTTP/TLS client and browser with real HTTP/1.1 servers,
   current certificate chains, chunked responses, redirects, and disconnects.
   Add checks for malformed input and size limits.
2. Complete and validate TCP before increasing concurrency or accepting pages
   with multiple resources.
3. Add a DOM, CSS selection, and layout that fit system memory and the
   framebuffer; then decide whether to port `libcss` and its NetSurf
   dependencies.
4. Design restricted syscall permissions and IPC to separate the browser from
   its renderer. Separate address spaces alone do not make a sandbox.
5. Add image loading, form submission, per-site permissions, and downloads
   when the transport and isolation model are ready.

Chromium is a reference for its renderer process and isolation, but its Linux
build system and dependencies do not fit WeLoveC's static freestanding ELFs
directly. litehtml is another CSS option, but it requires the C++ STL, which
this userland does not provide. The tarball preserves NetSurf license notices.
The build uses `libhubbub` and `libparserutils`, disables Iconv, and generates
the element table with `netsurf_make_element_types.pl` because the toolchain
does not require `gperf`.

New shortcuts: `[`/`]` change the selected link, Enter opens it, `h` goes back,
and `f` moves forward in history. `u`, `r`, the arrow keys, `j`/`k`, Space, `b`,
and `q` keep their existing behavior.

## Build status

The `build/userland/bin/browser/browser.elf` target compiled successfully with
the project's configured freestanding toolchain. It has not yet been exercised
in a VM or validated against real network traffic, so the browser remains
experimental.
