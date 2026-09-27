# wrozka-kanalozka

> **Proof of concept.** It works on the author's setup. It has not been tested broadly, has no enclosure or
> power design, and may change without notice. Use at your own risk.

A small ESP32-C3 device that changes your quad's VTX channel with one button press. You don't need to open the
ExpressLRS Lua menu. The device talks over ESP-NOW to the **ExpressLRS TX backpack** in your radio's TX module,
and ExpressLRS VTX Admin pushes the new channel to the quad.

Each press of the button cycles through four Raceband channels:

**R1 → R3 → R6 → R7 → R1 …**

Only the band and channel change. VTX power and pit mode stay as configured on the TX module.

## How it works

```
[wrozka-kanalozka] --ESP-NOW--> [TX backpack] --serial--> [ELRS TX module] --VTX Admin--> [RX] --> [FC] --> [VTX]
        ^                                                        |
        '------------------ echo (confirmation) -----------------'
```

1. When you press the button, the device sends an MSPv2 `MSP_SET_VTX_CONFIG` packet over ESP-NOW. The payload
   holds only the VTX table index.
2. The TX backpack only accepts ESP-NOW packets from the MAC address derived from the binding phrase. It
   forwards the packet to the TX module.
3. The TX module reads **only the band/channel byte** from the packet, saves it, and triggers a VTX Admin send
   to the receiver and flight controller.
4. When disarmed, the TX module sends the new VTX config back through the backpack. The device receives this
   echo and treats it as confirmation.

The device takes its MAC address from the binding phrase the same way ExpressLRS does: the first 6 bytes of
`md5('-DMY_BINDING_PHRASE="<phrase>"')`, with the lowest bit of the first byte cleared.

No custom ExpressLRS firmware is needed. The device uses the existing backpack feature that lets an ESP-NOW
device such as a race timer set a pilot's VTX channel.

## Hardware

- [Adafruit QT Py ESP32-C3](https://www.adafruit.com/product/5405)
- The onboard **BOOT button** (GPIO9) is the channel button
- The onboard **NeoPixel** (GPIO2) shows status

No extra wiring is needed. Power it from USB-C.

## Requirements

| Component | Minimum | Tested / recommended |
|---|---|---|
| ExpressLRS TX module | 3.0.0, **ESP32-based** | latest 3.6.x / 4.x |
| ExpressLRS TX backpack | **1.4.0** | latest 1.5.x |

- The TX module and backpack must use the **same binding phrase** as this device.
- The backpack must be in normal mode, not WiFi-update mode.
- VTX Admin must already work on your setup: a VTX table in Betaflight, a VTX controlled by the FC
  (SmartAudio/Tramp/MSP) and telemetry enabled. A quick test is the Lua **VTX Administrator → Send VTx**
  command. If that changes your channel, this device will too.
- Change channels while **disarmed**. The TX module doesn't echo while armed, and the FC may reject VTX changes.

## Build & flash

1. Install [PlatformIO](https://platformio.org/).
2. Set your binding phrase in `platformio.ini`:
   ```ini
   build_flags =
       '-DMY_BINDING_PHRASE="your binding phrase"'
   ```
3. Build, flash and open the serial monitor:
   ```sh
   pio run -t upload && pio device monitor
   ```
   At boot the device prints the MAC address it uses. You can check it against your phrase:
   ```sh
   python3 -c 'import hashlib;u=bytearray(hashlib.md5(b"-DMY_BINDING_PHRASE=\"your binding phrase\"").digest()[:6]);u[0]&=~1;print(":".join("%02X"%x for x in u))'
   ```

## Usage

Power on the radio with its TX backpack, and the device. For the first few seconds the device asks the
backpack for the current VTX setting, so the LED can show the current channel straight away. Press BOOT to
step to the next channel.

### LED

| LED | Meaning |
|---|---|
| Dim cyan / green / blue / magenta | Current channel is R1 / R3 / R6 / R7 |
| Dim white | Current channel unknown, or not one of the four presets |
| Fast blink in the preset colour | Change sent, waiting for the TX |
| Solid bright preset colour | The TX module confirmed the change |
| Preset colour alternating with orange | The backpack received it but no confirmation came back (e.g. armed) |
| Red blink | The backpack never acknowledged the packet, even after 3 tries |

A red blink usually means a wrong binding phrase, the backpack is off or in WiFi mode, or it's out of range.

### Notes

- Pressing the button quickly skips ahead. Only the channel you land on gets applied.
- **The BOOT button is a strapping pin.** Holding it while powering on or resetting puts the chip into
  download mode.
- The TX module turns VTX Admin on automatically, with the band set to Raceband.
- Goggles with a VRX backpack on the same binding phrase will follow the channel change.

## Customising

All settings are at the top of [`src/main.cpp`](src/main.cpp):

- `PRESET_NAMES` / `PRESET_IDX` set the channel list. Index = `(band - 1) * 8 + (channel - 1)`, with bands
  A=1, B=2, E=3, F=4, R=5, L=6. Only indices 0–47 are supported; ExpressLRS ignores raw frequencies here.
- `PRESET_COLORS`, `LED_DIM`, `LED_BRIGHT` set the LED look.
- `BUTTON_PIN` selects the button. Use any GPIO with a button to GND; the internal pull-up is enabled.

## Known limitations / ideas

- Tested only on the author's hardware. It has not been checked across TX modules, backpack variants or VTXs.
- The binding phrase is compiled in, so changing it means reflashing. A small web configuration page would fix
  this.
- There is no battery or low-power mode.
- The channel list is fixed at compile time.

## References

- [ExpressLRS](https://github.com/ExpressLRS/ExpressLRS): `src/src/tx_main.cpp` (`ProcessMSPPacket`),
  `src/lib/VTX/devVTX.cpp`
- [ExpressLRS Backpack](https://github.com/ExpressLRS/Backpack): `src/Tx_main.cpp`
  (`ProcessMSPPacketFromPeer`), PR [#95](https://github.com/ExpressLRS/Backpack/pull/95)
