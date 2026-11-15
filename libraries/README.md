# Vendored libraries

Built with `arduino-cli compile --libraries libraries ...`, so no install into
`Documents\Arduino\libraries` is needed.

| Library | Version | Source |
|---|---|---|
| Mozzi | 2.0.4 | https://github.com/sensorium/Mozzi |
| FixMath | 1.0.9 | https://github.com/tomcombriat/FixMath |

## Local changes

- `Mozzi/utility/FrequencyTimer2.cpp`: commented out a leftover debug
  `Serial.println(period);` in `setPeriodCPUCycles()`. Mozzi has a flat
  layout, so every one of its `.cpp` files is linked even when unused, and
  `dot_a_linkage` does not help. That one line pulls in Arduino's
  `HardwareSerial0` and its USART interrupt (`__vector_19`), which clashes with
  our UART audio interrupt. FrequencyTimer2 is only used by Mozzi's HIFI
  output mode, which this project does not use.
- `examples/`, `extras/` (Mozzi) and `examples/`, `docs/` (FixMath) were deleted
  to keep the repository small. The build doesn't use them.
