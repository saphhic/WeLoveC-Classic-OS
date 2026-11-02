# Base de drivers GPU

## Alcance actual

DisplayService tiene ahora un núcleo de descubrimiento independiente del driver
de cada GPU. Escanea PCI, enlaza los dispositivos que aparecen en el registro y
publica estado, capacidades, dirección PCI y BAR sin depender del escritorio.
La primera entrada es AMD Rembrandt `1002:1681`.

El sondeo de Rembrandt es deliberadamente pasivo. Lee la identidad PCI, revisión
y BAR configurados; no cambia registros, no habilita bus mastering, no reinicia
la GPU y no mapea MMIO. Por ello la salida UEFI GOP sigue siendo propiedad del
firmware y el framebuffer actual continúa atendiendo la consola y el escritorio.
La detección no anuncia capacidades nativas que todavía no existen.

## Capas

- `gpu.hpp` define el contrato del servicio: estado del escaneo, lista de
  dispositivos, capacidades, estado de GOP y operaciones genéricas de modos y
  presentación.
- `gpu.cpp` realiza un escaneo PCI único, enlaza un driver por ID, administra su
  ciclo de vida y valida los argumentos de las llamadas comunes.
- `Drivers/gpu_driver.hpp` separa el ciclo de vida del dispositivo PCI de las
  operaciones propias de una GPU.
- `Drivers/registry.cpp` es el registro estático de drivers disponibles.
- `Drivers/amd/rembrandt.cpp` enlaza el ID solicitado y reporta recursos; sus
  operaciones de inicialización, modos y presentación quedan desactivadas hasta
  que exista implementación de hardware.
- `pci::read_bars` interpreta BAR de endpoint, puente y CardBus sin escribir en
  el espacio de configuración. Informa direcciones y tipos, no tamaños. Medir un
  BAR escribiendo unos puede interrumpir dispositivos activos y requiere un
  protocolo explícito de propiedad del dispositivo.

## Reglas para la siguiente etapa

1. Añadir un administrador de recursos MMIO que valide rangos, detecte
   solapamientos, aplique una política de caché única y revierta mapas parciales.
   No convertir una dirección BAR en un puntero mediante una conversión directa.
2. Definir la propiedad del dispositivo y el traspaso desde GOP antes de tocar
   el motor de pantalla. La ruta GOP debe seguir disponible si el inicio nativo
   falla.
3. Añadir carga y validación de firmware mediante el VFS antes de habilitar
   bloques que dependan de microcódigo.
4. Habilitar DMA y bus mastering solo después de tener buffers DMA, límites de
   direccionamiento y protección de IOMMU definidos. No aceptar comandos GPU de
   procesos sin validación y aislamiento.
5. Implementar por etapas inicialización del motor, memoria GPU, colas y fences;
   después añadir modos/conectores y conectar presentación al compositor.
6. Publicar una capacidad solo cuando la operación correspondiente funcione y
   tenga una ruta de error que conserve la pantalla anterior.

## Cómo extenderlo

Un nuevo driver aporta un `Driver` con IDs PCI, función `probe` y, cuando esté
listo, callbacks de inicialización, apagado, enumeración/configuración de modos
y presentación. Se añade al arreglo estático de `Drivers/registry.cpp`. `probe`
debe permanecer de solo lectura; la inicialización de hardware se ejecuta por
separado y el núcleo solo marca el dispositivo como inicializado si devuelve
`WELOVEC_GPU_OK`.

Las APIs de modos y presentación devuelven `WELOVEC_GPU_UNSUPPORTED` para
Rembrandt por ahora. El estado del sistema informa GOP y la resolución actual,
sin atribuir GOP a una GPU concreta: el protocolo de arranque aún no entrega el
BDF de la tarjeta que proporcionó el framebuffer.
