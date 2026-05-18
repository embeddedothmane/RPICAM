Programme C++ pour Raspberry Pi Zero 2 W + Camera Module 3

Principe
- Un thread capture une photo JPEG toutes les 5 secondes.
- Chaque image est horodatée et stockée localement dans pending/.
- Un autre thread envoie les images vers un drive cloud.
- Après upload réussi, le fichier est déplacé dans uploaded/.
- En cas d'échec d'upload, le fichier reste/revient en attente et sera retenté.

Pourquoi ce design
- Capture: on s'appuie sur rpicam-still, l'outil officiel Raspberry Pi basé sur libcamera.
- Upload: on s'appuie sur rclone, très pratique et indépendant du fournisseur cloud.
- Orchestration: en C++17 pour un service robuste et léger.

Dépendances à installer
sudo apt update
sudo apt install -y rpicam-apps rclone build-essential

Configuration cloud
1) Lance: rclone config
2) Crée un remote, par exemple "gdrive"
3) Choisis Google Drive ou un autre backend supporté
4) Mets ensuite ce remote dans le code:
   std::string rcloneRemote = "gdrive:camera_uploads";

Compilation
g++ -std=c++17 -O2 -pthread capture_upload_rpi_cam3.cpp -o capture_upload_rpi_cam3

Exécution
./capture_upload_rpi_cam3

Répertoires créés
/home/pi/captures_cam3/
├── pending/
├── uploaded/
└── failed/

Notes autofocus
- Camera Module 3 a un autofocus matériel.
- Le programme appelle rpicam-still avec --autofocus-mode auto.
- Pour les documents, commence avec un bon éclairage diffus.
- Si tu veux figer un petit mouvement, règle shutterUs dans le code.
  Exemple: 8000 = 8 ms.

Réglages utiles pour la lecture de documents
- width=2304 height=1296 : plus léger sur Pi Zero 2 W
- si tu veux plus de détail: 4608x2592, mais plus lent
- timeoutMs: 1200 à 2000 selon le temps laissé à l'AF/AE/AWB
- shutterUs: 5000 à 10000 si sujet légèrement mobile et lumière suffisante
- gain: laisse à 0 au début

Pourquoi je recommande rclone au lieu de coder Google Drive directement en C++
- moins de complexité OAuth2
- beaucoup plus simple à maintenir
- change facilement de Google Drive vers OneDrive, Dropbox, S3, etc.
- le programme C++ reste concentré sur sa logique métier

Évolution possible
- service systemd
- fichier de config .json/.ini
- compression/redimensionnement
- checksum avant/après upload
- rotation des logs
