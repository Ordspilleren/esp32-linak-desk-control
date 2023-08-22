# ESP32 LINAK Desk Controller

This project uses an ESP32 to read and write data to the control box found in many LINAK standing desks.

For now, the following is implemented:
- Reading the current height of desk.
- Setting the current height to a specific value.
- Exposing the above to an MQTT broker.

## Hardware

In order to interface with the desk, a custom circuit is needed. LINAK uses LIN Bus to communicate over the RJ45 port on the desk which operates at 12V and uses a single wire for RX and TX. We need to bring this down to ~3.3V and add a transistor to bring it down to GND when doing TX.

Full credit for the schematic goes to [github.com/stevew817/linak_desk](https://github.com/stevew817/linak_desk).

![](./schematic.png)

# References
- [github.com/stevew817/linak_desk](https://github.com/stevew817/linak_desk): The general approach to reading the LIN Bus frames and the schematic have been borrowed from here.