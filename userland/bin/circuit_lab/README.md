# Circuit Lab

Aplicacion independiente de usuario para WeLoveC Classic. El Makefile genera
`build/userland/bin/circuit_lab/circuit_lab.elf`; al incluir ese ELF en la
imagen de usuario, aparece como programa del escritorio.

## Uso

- El circuito de ejemplo se abre al iniciar. Pulsa **SIMULAR** y cambia el
  interruptor con la herramienta **Elegir** para ver el efecto en la bombilla.
- Elige bateria, resistor, bombilla o switch y pulsa en la cuadrícula para
  colocar una pieza. Arrastra una pieza para moverla.
- Elige **Cable** y pulsa los dos terminales que quieras unir.
- Pulsa **SIMULAR** para resolver el circuito de corriente continua. Las piezas
  muestran corriente en mA; los nodos con tension aparecen en verde.
- **BORRAR** limpia el circuito. **DEMO** restaura el circuito didactico.
- Atajos: `1` bateria, `2` resistor, `3` bombilla, `4` switch, `W` cable,
  `S` seleccionar, `Espacio` simular, `C` limpiar y `Supr` borrar la pieza.

El modelo actual usa una bateria ideal de 9 V, un resistor de 100 ohmios, una
bombilla modelada como 220 ohmios y un switch abierto/cerrado.
