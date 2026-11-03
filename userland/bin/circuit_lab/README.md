# Circuit Lab

Standalone user application for WeLoveC Classic. The Makefile builds
`build/userland/bin/circuit_lab/circuit_lab.elf`; the ELF is added to the user
image and appears as a desktop program.

## Usage

- A sample circuit opens at startup. Press **SIMULATE**, then toggle the
  switch with the **Select** tool to see its effect on the lamp.
- Choose a battery, resistor, lamp, or switch, then click the grid to place a
  part. Drag a part to move it.
- Choose **Wire**, then click the two terminals to connect.
- Press **SIMULATE** to solve the DC circuit. Parts show current in mA; nodes
  with voltage are shown in green.
- **CLEAR** clears the circuit. **DEMO** restores the sample circuit.
- Shortcuts: `1` battery, `2` resistor, `3` lamp, `4` switch, `W` wire,
  `S` select, `Space` simulate, `C` clear, and `Del` remove a part.

The current model uses an ideal 9 V battery, a 100-ohm resistor, a lamp modeled
as 220 ohms, and an open/closed switch.
