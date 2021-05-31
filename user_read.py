with open("/dev/mypci", "rb") as f:
	raw = int.from_bytes(f.read(2), byteorder='little', signed=True)

print(str(raw * 10 / (2047*16))+" V")
