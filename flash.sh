# Flash firmware to device
#
# Dependencies: esptool.py

esptool.py --port /dev/ttyUSB0 write_flash 0x40000 ../bin/eagle.irom0text.bin 0x00000 ../bin/eagle.flash.bin