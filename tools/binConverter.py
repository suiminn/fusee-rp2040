#!/usr/bin/env python3
"""
This code was shamelessly stolen from https://github.com/blockfeed/sam-fusee-launcher-internal/
"""
import os
import argparse
import sys


def printProgressBar(progress):
    i = int(progress * 20)
    sys.stdout.write('\r')
    sys.stdout.write("[%-20s] %d%%" % ('='*i, 5*i))
    sys.stdout.flush()

def openFileToByte_generator(filename , chunkSize = 128):
    fileSize = os.stat(filename).st_size
    readBytes = 0.0
    with open(filename, "rb") as f:
        while True:
            chunk = f.read(chunkSize)
            readBytes += len(chunk)
            if fileSize:
                printProgressBar(readBytes/float(fileSize))
            if chunk:
                for byte in chunk:
                    yield byte
            else:
                break


parser = argparse.ArgumentParser(description="Convert a binary blob into a C include file.")
parser.add_argument("input", help="binary file to convert")
parser.add_argument("--symbol", default="payload", help="C array symbol name")
parser.add_argument("--section", default="payloads", help="RP2040 flash section name")
args = parser.parse_args()

fileIn = args.input


base = os.path.splitext(fileIn)[0]
fileOut =  base + ".hex"

stringBuffer = "\t"
countBytes = 0
print("reading file: " + fileIn)

for byte in openFileToByte_generator(fileIn,16):
    countBytes += 1
    stringBuffer += f"0x{byte:02X}, "
    if countBytes%16 == 0:
    	stringBuffer += "\n\t"



stringBuffer = (
    "#include <pico/platform.h>\n"
    f"const uint8_t __in_flash(\"{args.section}\") {args.symbol}[] = {{\n"
    + stringBuffer
    + "\n};"
)

print("\nwriting file: " + fileOut)
with open(fileOut, "w") as text_file:
    text_file.write(stringBuffer)

print("finished")
