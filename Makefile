devola-mqtt:	devola-mqtt.c
		cc -o devola-mqtt devola-mqtt.c -lmosquitto

clean:		
		rm devola-mqtt
