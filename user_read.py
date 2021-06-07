#!/usr/bin/env python3

with open("/dev/mypci", "rb") as f:
	raw = int.from_bytes(f.read(2), byteorder='little', signed=True)

#print(str(raw * 10 / (2047*16))+" V")
print(str(raw * 10 / 32768)+" V")
