#!/bin/bash

set -e

echo "[*] Removing module..."
sudo rmmod asyncmsg || true

echo "[*] Cleaning build..."
make clean

echo "[*] Building module..."
make

echo "[*] Inserting module..."
sudo insmod asyncmsg.ko

echo "[*] Setting permissions..."
sudo chmod 666 /dev/asyncmsg

echo "[+] Done! Module reloaded and ready."
