capture_on_demande.cpp

But:
- s'abonne a un broker MQTT
- attend une demande JSON
- si la commande demandee est capture_on_demand, lance une capture
- conserve les memes chemins et les memes parametres camera que capture_last_image.cpp
- ecrit la capture unique dans /var/lib/rpicam-http/LAST_CAPTURE.jpg
- archive aussi chaque capture dans /home/pi/captures_cam3/pending/img_*.jpg

Dependances:
sudo apt update
sudo apt install -y g++ rpicam-apps libmosquitto-dev mosquitto-clients

Compilation:
g++ -std=c++17 -O2 -pthread capture_on_demande.cpp -lmosquitto -o capture_on_demande

Exemple de lancement:
./capture_on_demande \
  --mqtt-host 192.168.1.10 \
  --mqtt-port 1883 \
  --mqtt-topic rpicam/capture/request \
  --output-dir /var/lib/rpicam-http \
  --output-name LAST_CAPTURE.jpg \
  --width 2304 \
  --height 1296 \
  --timeout-ms 700 \
  --autofocus-mode auto \
  --autofocus-range normal

Exemple de JSON minimal:
{"command":"capture_on_demand"}

Exemple de JSON avec paramètres a la racine:
{"command":"capture_on_demand","width":1920,"height":1080,"timeout_ms":500,"autofocus_mode":"auto","autofocus_range":"macro"}

Exemple de JSON avec objet params:
{"command":"capture_on_demand","params":{"width":1920,"height":1080,"timeout_ms":500,"autofocus_mode":"manual","lens_position":"3.33","shutterUs":8000,"gain":1}}

Exemple de publication MQTT:
mosquitto_pub -h 192.168.1.10 -t rpicam/capture/request -m '{"command":"capture_on_demand"}'

Ou:
mosquitto_pub -h 192.168.1.10 -t rpicam/capture/request -m '{"command":"capture_on_demand","params":{"width":1920,"height":1080,"timeout_ms":400,"autofocus_mode":"manual","lens_position":"3.33"}}'

Paramètres JSON supportés:
- width
- height
- timeout_ms
- autofocus_mode
- autofocus_range
- lens_position
- shutterUs
- gain

Remarques:
- La capture MQTT est sérialisée par un thread worker: une demande = une capture
- Le programme garde les mêmes chemins que ton capture_last_image.cpp
- Les paramètres JSON surchargent la configuration de base au moment de la demande
- Le champ command est obligatoire
