# Make your gas/water meter smart
For forcasting the whole gas and water costs it is necessary get the values from the meters provided by your utility company. Also you can easily calculate the actual expenses. Even if you did not get installed smart meters from your utility company it is common that the devices provide an impulse counter, simply a magnet. This can be used to record the consumption simply by counting the impulses. 

There are some projects included tasmota that provided impulse counters but I decided to create my own to have it specialzed to my purposes included a WebUI for gas and water.

## Featurelist
* MQTT publishing to get logging information
* Realtime clock for logging
* Guided setup to avoid storing WiFi password, etc. in the code

## [Hardware](docs/schema.pdf)
* Wemos D1 Mini ESP32
* STL for [case (thingiverse)](docs/Warmwasserpumpe.stl)
* [sensor case](docs/Warmwasserpumpe(2).stl)

![Case with components mounted](img/Case%20with%20components.JPG)

## Mounting
The reed sensor has to be mounted near to the magnnet. For ... gas meter I included as case, the sensor has to be mounted  near to the outer wall of the number mechanics.

# Software
On first start the thing will open an Access Point named "Gaszaehler" to provide the setup interface. The interface is still available after the device is connected so you can change everything later. But at first the firmware has to be uploaded. You can compile your own version or use the firmware provided in the releases.

1. Do the system configuration and set things name (hostname), AP password (if WiFi connection is lost) and WiFi credentials for your network.
2. MQTT configuration (optional)
   1. publish the following topics (folder structure can be changed):
      * "ht/gas/imp_counted": actual impulse count
3. NTP configuration to get RTC infos for logging (default is fine for german timezone)

![config page](img/opera_2022-10-31%20213941.png)