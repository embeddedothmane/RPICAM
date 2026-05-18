pending_sftp_sync_v2.cpp

But
- scanner le dossier pending
- ignorer les fichiers trop récents
- trier les fichiers par ordre du plus ancien last_write_time au plus récent
- envoyer un seul fichier à la fois en SFTP/SSH vers un VPS Linux
- si succès: déplacer vers uploaded
- si échec non-réseau: réessayer jusqu'à max_retries puis déplacer vers failed
- si le VPS/réseau est indisponible: attendre et reprendre sans consommer les retries
- le compteur de retry n'est gardé qu'en RAM, pour le fichier courant uniquement

Règle d'ordre
- En C++ standard sur Linux, la date de création n'est pas portable.
- Ce daemon utilise last_write_time.
- Donc l'ordre réel est: premier fichier le plus ancien en date de dernière écriture = premier envoyé.
- En cas d'égalité, départage par nom de fichier.

Logique exacte de boucle
- listing de fichiers dans pending
- filter out des fichiers "nouveau-nés"
- tri du plus vieux au plus récent
- prise du plus vieux fichier éligible
- upload de ce fichier
- si succès -> move vers uploaded
- si échec non-réseau -> retry jusqu'à 10 fois puis move vers failed
- si réseau/VPS down -> attente puis reprise
- puis on refait toute la boucle

Règle "nouveau-né"
- Un fichier est ignoré si moins de 5 secondes se sont écoulées depuis son dernier write
- Le délai est configurable via --stability-seconds
- Par défaut: 5 secondes

Compilation
g++ -std=c++17 -O2 pending_sftp_sync_v2.cpp -lcurl -o pending_sftp_sync_v2

Exemple avec tes valeurs
./pending_sftp_sync_v2 \
  --host 10.20.30.40 \
  --user root \
  --remote-dir /root/RPICAMM/upload \
  --pending-dir /home/pi/captures_cam3/pending \
  --uploaded-dir /home/pi/captures_cam3/uploaded \
  --failed-dir /home/pi/captures_cam3/failed \
  --private-key /home/pi/.ssh/id_ed25519_capture_sync \
  --public-key /home/pi/.ssh/id_ed25519_capture_sync.pub \
  --known-hosts /home/pi/.ssh/known_hosts \
  --scan-interval-s 2 \
  --retry-wait-s 10 \
  --offline-wait-s 15 \
  --max-retries 10 \
  --stability-seconds 5
