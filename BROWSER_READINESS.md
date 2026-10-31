# Estado del navegador web de WeLoveC Classic

WeLoveC Classic ya incluye un primer navegador de texto para el escritorio,
con HTTP y HTTPS sobre la red de userland. Es un prototipo compilado que
implementa obtención y lectura de páginas; todavía no es un navegador web
general ni está listo para sitios actuales que dependen de CSS, JavaScript o
contenido multimedia.

## Componentes implementados

- **Aplicación de escritorio:** `userland/bin/browser/main.c` se compila como
  `/userspace/bin/browser/browser.elf`. El escritorio lo descubre en su lista
  de programas y lo ejecuta en una ventana con entrada de teclado propia.
  Incluye edición de dirección, recarga, desplazamiento, historial de hasta 16
  páginas y navegación de teclado por enlaces.
- **URLs:** `userland/lib/web/url.c` analiza HTTP y HTTPS, hosts DNS ASCII,
  literales IPv6 y puertos; descarta fragmentos y resuelve referencias
  absolutas, relativas y de ruta. No implementa IDNA, canonicalización completa
  ni codificación porcentual completa.
- **Cliente HTTP:** `userland/lib/web/http.c` envía solicitudes GET, limita las
  cabeceras a 16 KiB y acepta cuerpos delimitados por `Content-Length` o
  `Transfer-Encoding: chunked`. Sigue hasta cinco redirecciones, admite
  referencias relativas y rechaza redirecciones de HTTPS a HTTP. No envía
  cookies ni credenciales, y marca las páginas HTTP como no cifradas. No acepta
  cuerpos delimitados solo por el cierre de la conexión ni codificaciones de
  contenido como gzip.
- **HTTPS:** el adaptador Mbed TLS requiere TLS 1.2 o posterior y verifica el
  certificado y el nombre de host con el paquete de CA del sistema. Los errores
  de certificado se muestran al usuario; no hay reintento silencioso por HTTP.
- **HTML:** `third_party/netsurf/netsurf-all-3.11.tar.gz` aporta el parser y
  tokenizador Hubbub de NetSurf. El callback del navegador consume etiquetas,
  atributos y texto decodificado; detecta el charset del documento, omite
  `script`, `style` y el contenido de `head`, inserta saltos para bloques,
  conserva espacios en `pre` y muestra el texto alternativo de imágenes. No se
  activa el constructor de árbol/DOM de Hubbub.
- **Enlaces:** recoge hasta 64 destinos HTTP(S), resuelve rutas relativas contra
  el primer `<base href>` válido o, si no existe, contra la URL final; antepone
  su número al texto. `[` y `]` seleccionan enlaces y Enter abre el
  seleccionado. No activa esquemas distintos de HTTP(S).
- **Texto plano:** las respuestas no HTML se muestran directamente. La
  aplicación limita cada cuerpo a 256 KiB.
- **Red:** usa DNS A/AAAA y TCP IPv4/IPv6 ya expuestos al userland. Cada
  solicitud es secuencial; no descarga recursos secundarios. Los errores del
  navegador identifican DNS, falta de ruta, falta de entropía del kernel,
  rechazo del puerto, timeout del handshake TCP, envío y recepción para ayudar
  a localizar fallos de red. Un timeout significa que no llegó respuesta al
  SYN; no demuestra por sí solo si el servidor, el puerto o un firewall causó
  el silencio.

TCP también necesita entropía segura para generar sus números de secuencia y
puertos locales. El kernel la obtiene del RNG de UEFI, de RDSEED/RDRAND o de un
dispositivo VirtIO RNG. En QEMU se puede añadir a la configuración existente
`-device virtio-rng-pci`; después hay que reiniciar la máquina virtual. El
controlador VirtIO RNG ya forma parte del kernel. No se sustituye esta fuente
por valores predecibles porque el mismo generador protege TLS y otras funciones
de seguridad.

HTTPS primero debe abrir TCP al puerto 443 y después negociar TLS. Que un sitio
solo sirva HTML no determina el protocolo: puede servir HTML por HTTP o HTTPS.
Si el servidor solo ofrece HTTP, se debe abrir `http://host/`; una URL `https://`
no cambia automáticamente al puerto 80 ni baja a HTTP.

## Límites actuales

1. **No hay motor web completo.** Hubbub se usa para tokenizar y analizar HTML,
   pero el navegador no construye DOM ni aplica las reglas de reparación del
   árbol HTML. Tampoco tiene layout CSS, JavaScript, fuentes escalables, carga
   de imágenes, audio/video, formularios ni envío de formularios. Las imágenes
   solo pueden mostrar su atributo `alt`.
2. **La interfaz sigue siendo básica.** No hay pestañas, marcadores,
   descargas, cookies, permisos por origen, configuración ni almacenamiento web.
   Los enlaces se limitan a HTTP(S) y el historial conserva como máximo 16 URL.
3. **El aislamiento no es suficiente para contenido hostil.** El navegador se
   ejecuta como proceso independiente del escritorio, pero no tiene sandbox ni
   permisos reducidos: el proceso conserva las capacidades normales de su
   usuario, incluida la red.
4. **El transporte requiere más trabajo.** El TCP actual usa stop-and-wait y
   procesa datos en orden; faltan control de congestión, escalado de ventana,
   segmentos fuera de orden y cierre FIN completo. También falta validar la
   interoperabilidad con pérdida y distintos servidores. La syscall de lectura
   ahora hace un sondeo no bloqueante antes de vaciar los bytes recibidos; sin
   ese sondeo el navegador nunca veía respuestas TCP aunque ICMP funcionara.
5. **El soporte web es intencionalmente acotado.** No hay IDNA, canonicalización
   completa de URL, compresión HTTP, respuestas delimitadas por cierre ni
   descargas mayores de 256 KiB en esta aplicación. El parser HTML no sustituye
   un navegador completo ni un motor de layout.

## Próximos pasos

1. Probar el cliente HTTP/TLS y el navegador con servidores HTTP/1.1 reales,
   cadenas de certificados actuales, respuestas chunked, redirecciones y
   desconexiones. Añadir pruebas para entradas malformadas y límites de tamaño.
2. Completar y validar TCP antes de aumentar la concurrencia o admitir páginas
   con varios recursos.
3. Añadir un árbol DOM, selección CSS y layout compatibles con la memoria y el
   framebuffer del sistema; decidir entonces si se porta `libcss` y sus
   dependencias de NetSurf.
4. Diseñar permisos de syscall e IPC acotado para separar navegador y
   renderizador. Los espacios de direcciones separados por sí solos no forman
   un sandbox.
5. Añadir carga de imágenes, envío de formularios, permisos por origen y
   descargas cuando el transporte y el modelo de aislamiento estén listos.

Chromium sirve como referencia para el proceso de red y el aislamiento por
renderizador, pero su proceso de compilación Linux y sus dependencias no encajan
directamente con los ELF estáticos freestanding de WeLoveC. litehtml es otra
posibilidad para CSS, pero requiere C++ STL, que este userland no proporciona.
El tarball conserva los avisos de licencia de NetSurf; el build usa los fuentes
de `libhubbub` y `libparserutils`, desactiva Iconv y genera la tabla de
elementos con `netsurf_make_element_types.pl` porque el toolchain no requiere
`gperf`.

Atajos nuevos: `[`/`]` cambian el enlace seleccionado, Enter lo abre, `h` vuelve
atrás y `f` avanza en el historial. `u`, `r`, flechas, `j`/`k`, Space, `b` y `q`
conservan sus funciones anteriores.

## Revisión de compilación

El objetivo `build/userland/bin/browser/browser.elf` se compiló correctamente
con el toolchain freestanding configurado en el proyecto. Aún no se han hecho
pruebas de ejecución en una VM ni validación de interoperabilidad con tráfico
real; por ello el navegador debe considerarse experimental.
