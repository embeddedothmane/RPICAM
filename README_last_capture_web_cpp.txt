Projet: capture camera + page web avec derniere image

Choix technique:
- J'ai choisi C++ pour les 2 programmes.
- Le programme de capture appelle rpicam-still.
- Le serveur HTTP est un petit serveur socket autonome, sans framework.

Fichiers:
- capture_last_image.cpp
- last_capture_server.cpp

Installation:
sudo apt update
sudo apt install -y g++ rpicam-apps

Compilation:
g++ -std=c++17 -O2 -pthread capture_last_image.cpp -o capture_last_image
g++ -std=c++17 -O2 -pthread last_capture_server.cpp -o last_capture_server

Dossier de travail:
sudo mkdir -p /var/lib/rpicam-http
sudo chown $USER:$USER /var/lib/rpicam-http

Lancer la capture:
./capture_last_image --output-dir /var/lib/rpicam-http --interval-ms 1000 --width 1920 --height 1080 --timeout-ms 700 --autofocus-mode auto --autofocus-range normal

Lancer le serveur web:
./last_capture_server --public-dir /var/lib/rpicam-http --port 8080

Ouvrir dans le navigateur:
http://IP_DU_RPI:8080/

Notes pratiques:
- Le fichier affiche toujours la derniere capture sous le nom LAST_CAPTURE.jpg.
- Le programme capture ecrit d'abord dans un fichier temporaire, puis fait un rename atomique vers LAST_CAPTURE.jpg.
- Le navigateur recharge l'image toutes les 1 seconde avec un parametre anti-cache.
- Si 1 capture/seconde est trop ambitieux sur Pi Zero 2 W avec autofocus, reduis la resolution, augmente timeout-ms, ou passe en focus manuel.
- Pour des documents a distance quasi fixe, le focus manuel peut etre plus stable que l'autofocus.

Exemple focus manuel pour ~30 cm:
30 cm = 0.30 m => environ 3.33 dioptres
Exemple:
./capture_last_image --output-dir /var/lib/rpicam-http --interval-ms 1000 --width 1920 --height 1080 --timeout-ms 400 --autofocus-mode manual --lens-position 3.33

Option utile pour objets proches:
--autofocus-range macro

Arret:
Ctrl+C sur chaque programme
