pending_sftp_sync.cpp

But
- scanner le dossier pending
- trier les fichiers par ordre du plus ancien last_write_time au plus récent
- envoyer un seul fichier à la fois en SFTP/SSH vers un VPS Linux
- si succès: déplacer vers uploaded
- si échec non-réseau: réessayer jusqu'à max_retries puis déplacer vers failed
- si le VPS/réseau est indisponible: attendre et reprendre sans consommer les retries
- conserver un petit fichier d'état local pour ne pas perdre le compteur de retries au redémarrage

Règle d'ordre
- En C++ standard, la date de création n'est pas portable sur Linux.
- Ce daemon utilise last_write_time (date de modification) comme clé de tri.
- En cas d'égalité, il départage par nom de fichier.

Dépendances côté Raspberry Pi
sudo apt update
sudo apt install -y g++ libcurl4-openssl-dev openssh-client

Compilation
g++ -std=c++17 -O2 pending_sftp_sync.cpp -lcurl -o pending_sftp_sync

Exemple de lancement
./pending_sftp_sync \
  --host 203.0.113.10 \
  --user rpiupload \
  --remote-dir /srv/rpi_uploads \
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
  --stability-seconds 2

Options utiles
--host HOST
--user USER
--port 22
--remote-dir /srv/rpi_uploads
--pending-dir /home/pi/captures_cam3/pending
--uploaded-dir /home/pi/captures_cam3/uploaded
--failed-dir /home/pi/captures_cam3/failed
--state-file /home/pi/captures_cam3/.sftp_sync_state.tsv
--private-key /home/pi/.ssh/id_ed25519_capture_sync
--public-key /home/pi/.ssh/id_ed25519_capture_sync.pub
--known-hosts /home/pi/.ssh/known_hosts
--key-passphrase ...
--scan-interval-s 2
--retry-wait-s 10
--offline-wait-s 15
--connect-timeout-s 10
--transfer-timeout-s 120
--low-speed-limit 32
--low-speed-time-s 30
--max-retries 10
--stability-seconds 2
--quiet

Installation côté Raspberry Pi
1) Créer les dossiers
mkdir -p /home/pi/captures_cam3/pending
mkdir -p /home/pi/captures_cam3/uploaded
mkdir -p /home/pi/captures_cam3/failed

2) Générer une clé SSH dédiée
mkdir -p /home/pi/.ssh
chmod 700 /home/pi/.ssh
ssh-keygen -t ed25519 -f /home/pi/.ssh/id_ed25519_capture_sync -N ""

3) Ajouter la clé hôte du VPS dans known_hosts
ssh-keyscan -H 203.0.113.10 >> /home/pi/.ssh/known_hosts
chmod 600 /home/pi/.ssh/known_hosts

4) Tester la connexion SFTP
sftp -i /home/pi/.ssh/id_ed25519_capture_sync rpiupload@203.0.113.10

Installation côté VPS
1) Installer OpenSSH server si besoin
sudo apt update
sudo apt install -y openssh-server

2) Créer un utilisateur dédié
sudo adduser --disabled-password --gecos "" rpiupload

3) Créer le dossier de réception
sudo mkdir -p /srv/rpi_uploads
sudo chown -R rpiupload:rpiupload /srv/rpi_uploads
sudo chmod 755 /srv/rpi_uploads

4) Installer la clé publique de la Raspberry Pi
sudo mkdir -p /home/rpiupload/.ssh
sudo chmod 700 /home/rpiupload/.ssh
sudo tee -a /home/rpiupload/.ssh/authorized_keys < /tmp/id_ed25519_capture_sync.pub
sudo chmod 600 /home/rpiupload/.ssh/authorized_keys
sudo chown -R rpiupload:rpiupload /home/rpiupload/.ssh

5) Option de durcissement conseillée dans /etc/ssh/sshd_config
Match User rpiupload
    ForceCommand internal-sftp
    PasswordAuthentication no
    PubkeyAuthentication yes
    AllowTcpForwarding no
    X11Forwarding no

Puis
sudo systemctl restart ssh

Service systemd côté Raspberry Pi
1) Copier le binaire
sudo install -m 0755 pending_sftp_sync /usr/local/bin/pending_sftp_sync

2) Créer le service
sudo cp pending_sftp_sync.service /etc/systemd/system/pending_sftp_sync.service
sudo systemctl daemon-reload
sudo systemctl enable --now pending_sftp_sync.service

3) Vérifier
systemctl status pending_sftp_sync.service
journalctl -u pending_sftp_sync.service -f

Remarque
- Aucun code spécifique n'est nécessaire côté VPS.
- Un serveur OpenSSH avec accès SFTP suffit.
