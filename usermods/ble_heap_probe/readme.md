# ble_heap_probe (bench only -- DO NOT SHIP)

Measures internal-RAM cost of an Improv-shaped BLE GATT server next to WLED + the glowline usermod,
to size BLE Wi-Fi provisioning. Does no provisioning. Compiled only with `-D LUMEN_BLE_HEAP_PROBE`
(env `esp32s3_lumen_dev_BENCH_BLE_HEAP_DO_NOT_SHIP`); otherwise the file is empty.

Serial lines start with `bleprobe`. Stages:

- `S0 boot` -- before BLE starts
- `S1 ble_up` -- BLE initialised and advertising as `Lumen-probe`
- `S2 ble+wifi` -- Wi-Fi connected while BLE is still up (glowline's TLS handshake runs now)
- `S2b central_connected` / `central_disconnected` -- a phone connected to / left `Lumen-probe`
- `S3 ble_released` -- after `BLEDevice::deinit(true)`, `PROBE_BLE_HOLD_MS` after boot
- `S4 reconnect` -- Wi-Fi reconnect forced so glowline redoes its TLS handshake without BLE
- `tick` -- every 10 s

Each line: free internal heap, largest free internal block, minimum-ever free internal heap,
free PSRAM, configured LED count, current effect.
