#!/bin/bash
cd /home/pi/RPICAM

sleep 30
./last_capture_server --public-dir /var/lib/rpicam-http --port 8080 > /home/pi/log_cmd1.txt 2>&1 &
./capture_on_demande   --mqtt-host 185.137.122.221   --mqtt-port 1883   --mqtt-topic rpicam/capture/request   --width 2304   --height 1296   --timeout-ms 700   --autofocus-mode auto   --autofocus-range normal --duration 20 > /home/pi/log_cmd2.txt 2>&1 &
commande3 > /home/pi/log_cmd3.txt 2>&1 &