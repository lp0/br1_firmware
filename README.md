# Device

* Board: Generic ESP8266 Module
* Flash Mode: DIO
* Flash Frequency: 40MHz
* CPU Frequency: 160MHz
* Flash Size: 4M (3M SPIFFS)
* Reset Method: ck
* Programmer: AVRISP mkII


# ESP8266 Board

* Link ~/.arduino15/packages/esp8266/hardware/esp8266/2.1.0-rc1 to custom libraries


# Configuration Mode

The firmware will enter configuration mode if the EEPROM storage is empty, or
if the button is held during reset.

The device will configure itself as an open access point with a name in the
format *ws2812-xxxxxx*.

Connect to the access point and browse to http://192.168.4.1/ to configure the
desired wifi settings and the number of LEDs to be addressed.

After updating the settings, reset the device to enter normal operation.
