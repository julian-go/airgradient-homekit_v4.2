# airgradient-homekit

Experimental Apple HomeKit-compatible firmware for [airgradienthq/arduino](https://github.com/airgradienthq/arduino), based on [Mixiaoxiao/Arduino-HomeKit-ESP8266](https://github.com/Mixiaoxiao/Arduino-HomeKit-ESP8266/).


## Usage

* Load the DIY_PRO_V3_7 example
* Set the CPU Frequency (in "Tools") to 160 MHz.
* Upload
* In the "Home" app, add an accessory, click "More options...".
* The "AirGradient DIY PRO" should show up in the list of nearby devices.
* Pair with code 1111-1111.


## Issues

* The ESP8266 is easily running out of heap memory, causing frequent reboots.


## Screenshots

![Screenshot](homekit_airgradient1.png)
![Screenshot](homekit_airgradient2.png)
