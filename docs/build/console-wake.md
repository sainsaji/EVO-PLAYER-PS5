# Waking the PS5 after a power cut (pretend controller)

**Status (2026-10-07): plan and preparation only. No board bought, no firmware
written, nothing tried on the console.**

## The problem

Power cuts are frequent where the dev console lives. A cut leaves the console
fully off, and the jailbreak is gone with it. Nothing in the repo can start a
console that is off.

## What is known

| | |
|---|---|
| A paired DualSense's **PS button powers on a console that is off** | confirmed first-hand by the owner, and PlayStation's support page lists the same two methods (PS button on a paired controller, power button) for rest mode and off |
| **Network wake** (Remote Play / chiaki-ng / `ps5-remoteplay wake`) | needs **rest mode**; a console that is off does not answer (the monitor-switching project documents this) |
| Console with **no AC power** | cannot be woken by anything |
| A Pico in the console's **USB port** | out: the ports only have power in rest mode |

So the route that reaches an off console is Bluetooth: something that looks to
the console like its paired controller and presses "PS".

## Chosen approach

A small board that is a Bluetooth **device** (not a host), pairs with the console
once as a controller, and later re-connects to it on command from the PC. No
pusher, no wiring, the real controller stays untouched.

**Board.** It must speak Bluetooth *Classic* (BR/EDR); a DualSense does not use
low energy. That means an **original ESP32** (ESP32-WROOM-32 or WROVER dev board)
or a **Raspberry Pi Pico W / Pico 2 W**. The ESP32-S3, -C3 and -C6 are
low-energy only and will not work. About $5-10.

**Control from the PC.** A one-line command (`wake`) over the board's USB serial
port, or over Wi-Fi on the Pico W / ESP32. Keep the board on a phone charger or
the PC's USB port so it survives the cut and comes back by itself.

## What has to be proved, in this order

Stop at the first "no".

1. **Does the console pair with it?** With the console on, add it under
   Settings -> Accessories -> General -> Bluetooth Accessories (the menu names
   may differ on FW 12.70). The board presents itself as a "Wireless Controller"
   with Sony's vendor and product id, the controller class of device, and the
   DualSense HID report descriptor. Unknown: the console may demand the
   controller authentication challenge and refuse a copy.
2. **Does it wake the console?** Turn the console off from the power menu (not
   rest mode), wait a minute, then have the board connect to the console's
   address the way a controller does. Success is only "the console turns on". It
   does not matter if the console drops the fake controller afterwards.
3. **Does it survive a power cut?** Cut the board's and the console's power,
   restore both, and repeat step 2 without re-pairing. The board must keep its
   pairing key in flash.

If step 1 fails because of authentication, the options are to answer the
challenge by relaying it to a real DualSense, or to give up on this route.

## Preparation that needs no board

- `tools/dualsense_dump.py` reads the report descriptor and the read-only feature
  reports from a real DualSense over **USB** and saves them to
  `output/dualsense/` (git-ignored; it contains the serial and Bluetooth
  address). Run it with the owner's controller plugged in by cable. **Do not
  pair that controller to the PC over Bluetooth**: that would replace its PS5
  pairing.
- The Bluetooth service records (SDP) and class of device of a real controller
  also need capturing, from a Linux machine or a Bluetooth sniffer. Not done.

## Reference projects

All are the *host* side (a board connecting to a real controller) or USB-side
emulators; none is a Bluetooth controller imitation for a console. Licences not
checked.

- [usedbytes/picow_ds4](https://github.com/usedbytes/picow_ds4) - DualShock 4 handling on a Pico W (BTstack).
- [rafaelvaloto/Pico_W-Dualsense](https://github.com/rafaelvaloto/Pico_W-Dualsense) - DualSense over Bluetooth Classic on a Pico W.
- [StryderUK/BluetoothHID](https://github.com/StryderUK/BluetoothHID) - ESP32 Bluetooth HID library with a DualShock 4 example.
- [Bluepad32](https://bluepad32.readthedocs.io) - ESP32 / Pico W gamepad host library.
- [wiredopposite/OGX-Mini](https://github.com/wiredopposite/OGX-Mini) and [GP2040-CE](https://gp2040-ce.info) - Pico USB controller firmware that lists PS5 support (USB, not Bluetooth).

## Risks

- The console may reject an unauthenticated "controller" (the likeliest failure).
- A mistake could leave the real controller needing a USB re-pair to the PS5.
  Annoying, not damaging. The board gets its own Bluetooth address, so the real
  controller's pairing is not touched.
- Nothing here helps a console with no AC power. A UPS does, and is still worth
  buying.

## Record results here

| Date | Step | Result |
|---|---|---|
| | | |
