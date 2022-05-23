# ESP-managed fishtank

This project is for automation of my paludarium.
## Hardware

I use an ESP32 development board soldered to perfboard and connected to four
MOSFET "drive modules".  Each drive module uses PWM to modulate the 12v power
that goes to each of my LED tank lights, dimming their output. The board also
connects to a DS18B20 temperature sensor board and probe. I use a USB cable to
power the ESP32 and standard 12v DC power supplies for the lights.

### BoM

[ESP32 Dev Board](https://www.amazon.com/dp/B0924G5STN?psc=1&ref=ppx_yo2ov_dt_b_product_details)
[Perfboard](https://www.amazon.com/dp/B072Z7Y19F?psc=1&ref=ppx_yo2ov_dt_b_product_details)
[Temperature Probe](https://www.amazon.com/dp/B09TXP645Y?psc=1&ref=ppx_yo2ov_dt_b_product_details)
[MOSFET Drivers](https://www.amazon.com/dp/B07NWD8W26?ref=ppx_yo2ov_dt_b_product_details&th=1)

## LED Fishtank Light Dimmer Control

Dimmers are changed via MQTT commands, either to set them to a specific
brightness or to set a ramp up/down target and time to reach the target for
smooth transitions. Status of the PWM dimmers are reported out via MQTT
messages as well.

### Brightness vs. Duty

"Duty" is the percentage of time when the PWM is "high" versus "low". 100%
duty would mean the PWM signal would be high continuously, and 0% would be
low. Duty is expressed, internally, as an integer number between 0-8191
(inclusively) with 8191 being 100% duty cycle. _Note:_ that as duty increases
the perceived brightness increases but not at a 1:1 ratio. This code includes
a simple "gamma correction" algorithm to map 50% "brightness" to a duty cycle
that would reasonably produce that requested brightness. The performance
of this correction may depend on your lights and certainly is not
confirmed scientifically.

### MQTT Commands

_prefix_/set/_dimmer #_/brightness : _brightness_ - Sets the brightness (0-8191,
0 being completely dark, 8191 being full brightness) for this dimmer #.

_prefix_/set/_dimmer #_/duty : _duty_ - Sets the explicit duty cycle (0-8191,
0 being 0%, 8191 being 100%) for this dimmer #.

_prefix_/set/_dimmer #_/power : "ON" or "OFF" - Turns the light on ("ON") or off ("OFF").

_prefix_/set/_dimmer #_/ramp : _brightness_ + " " + _seconds_ - Ramps the
brightness from the current setting to the target over a period of N seconds.

### MQTT Telemetry

_prefix_/status/_dimmer #_/power : "ON" or "OFF"

_prefix_/status/_dimmer #_/brightness : _brightness_ - 0-8191, with 0 being
fully off and 8191 being full brightness.

## Temperature Monitoring

This project also monitors a temperature sensor. An MQTT command instructs the
ESP32 to measure the temperature and the temperature is published
via an MQTT message.

### MQTT Commands

_prefix_/get/temp : "" - Requests telemetry for the temperature probe.

### MQTT Telemetry

_prefix_/temp : _temp in C_

## OTA Updates

An update to the firmware can be initiated via an MQTT message, which causes
the ESP32 microcontroller to connect to a URL to download an updated firmware.

### MQTT Commands

_prefix_/ota : "" - Requests an OTA update from the configured URL.
