# Design decisions

For the figures behind these decisions, see [MEASUREMENTS.md](MEASUREMENTS.md).

| Decision | Reason |
|---|---|
| Audio comes from the UART TX pin (PD1), in 8N1 mode at 1.25 Mbaud. | MIDI OUT is wired there, and PD1 has no PWM. SPI master mode would drive XCK (PD4), the USB pull-up pin. |
| Each 10-bit frame is a 9-level PWM cell; 8 frames make one sample at 15625 Hz. | The fixed start and stop bits let each frame set only one average level. |
| Second-order noise shaping per frame, in 8-bit assembly with TPDF dither. | Matches 8-bit PWM noise. The C version used a third of the CPU, and without dither the shaper whistles on silence. |
| The UART interrupt is naked assembly. | It runs at 125 kHz. |
| The mix peaks at −12 dB of full scale. | Full scale clips the line input. A cable attenuator would recover the SNR. |
| No Mozzi, Arduboy2, `Serial`, EEPROM, or USB MIDI during audio. | Saves CPU and RAM, keeps the UART vector free, leaves other sketches' EEPROM data alone, and avoids V-USB blocking the UART interrupt. |
| No function calls or 32-bit math in the audio path, and the ring is refilled fully before each UI step. | Otherwise audio drops out under load. |
