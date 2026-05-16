Programme Raspberry Pi Zero 2 W + Camera Module 3

Fichiers:
- capture_drive_uploader.py
- requirements-rpi-cam-drive.txt

Choix du langage:
J'ai choisi Python, pas C/C++, parce que pour ton besoin précis:
- Picamera2 est le chemin le plus simple côté Raspberry Pi Camera Module 3 + autofocus
- le client Google Drive officiel en Python est direct à utiliser
- le multi-threading ici est simple et largement suffisant

Installation recommandée sur Raspberry Pi OS Bookworm:
sudo apt update
sudo apt install -y python3-picamera2 python3-libcamera python3-pip
pip3 install --break-system-packages -r requirements-rpi-cam-drive.txt

Contenu du requirements:
- google-api-python-client
- google-auth-httplib2
- google-auth-oauthlib

Préparation Google Drive:
1) Crée un projet Google Cloud
2) Active Google Drive API
3) Configure l'écran de consentement OAuth
4) Crée un client OAuth de type "Desktop app"
5) Télécharge le fichier JSON et renomme-le en credentials.json
6) Copie ce fichier ici:
   /home/pi/captures_cam3/credentials.json

Dossier Drive cible:
- ouvre le dossier Google Drive voulu
- récupère son ID dans l'URL
- remplace DRIVE_FOLDER_ID dans le script

Premier lancement:
python3 capture_drive_uploader.py

Au premier lancement:
- le script demandera l'autorisation Google
- il créera token.json dans /home/pi/captures_cam3/

Comportement:
- capture une image toutes les 5 secondes
- autofocus avant chaque photo
- stocke d'abord dans pending/
- un autre thread envoie vers Google Drive
- après succès, le fichier va dans uploaded/

Arborescence locale:
 /home/pi/captures_cam3/
 ├── credentials.json
 ├── token.json
 ├── capture_upload.log
 ├── pending/
 ├── uploaded/
 └── failed/

Conseils pratiques pour ton cas "photo nette de feuille A4 / texte":
- monte la caméra de façon fixe
- mets un bon éclairage diffus
- commence à 20-25 cm de distance avec la Camera Module 3 standard
- si le Zero 2 W est trop lent en 4608x2592, passe IMAGE_SIZE à (2304, 1296)

Lancement auto au boot:
je peux aussi te préparer un service systemd si tu veux.
