# devola-mqtt

Devola are a UK company who make (among other things) Tuya-enabled glass panel heaters. There's a few drawbacks with Tuya implementation:

- It can be hard to monitor and control from a Home Automation server like Home Assistant or Open Hab as it doesn't talk MQTT to a local server. Some partial solutions exist (like tuya-mqtt) but they're complex to set up and might not be possible forever
- It's reliant on the Tuya control server in the cloud, and the Tuya or SmartLife App
- You might not be comfortable with your data being sent to a cloud server
- Ambient temperature is only sent by the stock Tuya firmware when the heater is on. It would be nice to send it all the time so you can monitor temperature in a room even when the heater is off

You can flash the ESP8266 chip that's onboard with the open source Tasmota firmware. But the problems don't end there ... the MCU and the stock firmware use a completely non-standard protocol, sending all of the parameters (power status, mode, setpoint, temperature, childlock and timer) in one 21-byte hex string. It's not easy to decompose that string into discrete parameters using Tasmota, or in a Home Automation server.

That's where devola-mqtt comes in. This translates between the hex string that the MCU uses and discrete parameters. It's driven by a simple "topics.conf" file that maps the topic name of the physical heater to a new name of a virtual heater with discrete parameters that presents these to your Home Automation server in a typical Tasmota style.

So you get MQTT messages like "stat/office-heater/POWER ON" or "stat/office-heater/SETPOINT 20" and can send messages like "cmnd/office-heater/POWER OFF". Heater mode is sent as a simple numeric - 1=low, 2=high, 3=anti-frost - as you will likely want to translate these to text in your HA server.

Currently, much of the detail of the config is in #defines at the head of the source file. At present it assumed you're using a wired ethernet connection, and that your MQTT broker is on the same server at the default port. Ideally, this would all be in command line options and/or a config file. But that's kind of gilding the lily. 

Keen lily gilders are invited to make a PR and add that functionality.
