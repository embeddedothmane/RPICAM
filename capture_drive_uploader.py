#!/usr/bin/env python3
"""
capture_drive_uploader.py

Raspberry Pi Zero 2 W + Camera Module 3:
- capture une photo toutes les 5 secondes
- autofocus avant chaque prise
- sauvegarde locale avec horodate
- envoi asynchrone vers Google Drive dans un autre thread
- après upload réussi, déplace le fichier vers le dossier "uploaded"

Testé conceptuellement pour Picamera2 + Google Drive API.
"""

import os
import time
import signal
import queue
import shutil
import logging
import threading
from pathlib import Path
from datetime import datetime

from picamera2 import Picamera2
from libcamera import controls

from google.auth.transport.requests import Request
from google.oauth2.credentials import Credentials
from google_auth_oauthlib.flow import InstalledAppFlow
from googleapiclient.discovery import build
from googleapiclient.http import MediaFileUpload


# =========================
# Configuration
# =========================
BASE_DIR = Path("/home/pi/captures_cam3")
PENDING_DIR = BASE_DIR / "pending"
UPLOADED_DIR = BASE_DIR / "uploaded"
FAILED_DIR = BASE_DIR / "failed"
LOG_FILE = BASE_DIR / "capture_upload.log"

# Côté Google Drive
# 1) mets ici l'ID du dossier Drive cible
#    exemple: https://drive.google.com/drive/folders/XXXXXXXXXXXX
DRIVE_FOLDER_ID = "PUT_YOUR_DRIVE_FOLDER_ID_HERE"

# 2) place credentials.json dans BASE_DIR
#    ce fichier vient d'un client OAuth "Desktop app" dans Google Cloud
CREDENTIALS_FILE = BASE_DIR / "credentials.json"
TOKEN_FILE = BASE_DIR / "token.json"

# Scope minimal pour créer/uploader des fichiers que l'application gère
SCOPES = ["https://www.googleapis.com/auth/drive.file"]

CAPTURE_INTERVAL_S = 5.0

# Résolution:
# - 4608x2592 = max Camera Module 3 (plus détaillé, plus lourd)
# - si le Pi Zero 2 W est trop lent -> descendre à (2304, 1296)
IMAGE_SIZE = (4608, 2592)

# Temps pour laisser AE/AWB/AF se stabiliser au démarrage
WARMUP_S = 2.0

# Si True, on garde une mise au point auto ponctuelle avant chaque photo
USE_AUTOFOCUS_CYCLE = True

# Si False, le thread upload laisse les fichiers dans pending même après upload
MOVE_AFTER_UPLOAD = True

# Délai de réessai en cas d'échec réseau / Drive
RETRY_DELAY_S = 15.0


# =========================
# Logging
# =========================
def setup_logging():
    BASE_DIR.mkdir(parents=True, exist_ok=True)
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s | %(levelname)s | %(threadName)s | %(message)s",
        handlers=[
            logging.FileHandler(LOG_FILE),
            logging.StreamHandler()
        ],
    )


# =========================
# Google Drive
# =========================
def get_drive_service():
    creds = None

    if TOKEN_FILE.exists():
        creds = Credentials.from_authorized_user_file(str(TOKEN_FILE), SCOPES)

    if not creds or not creds.valid:
        if creds and creds.expired and creds.refresh_token:
            logging.info("Refresh du token Google Drive...")
            creds.refresh(Request())
        else:
            if not CREDENTIALS_FILE.exists():
                raise FileNotFoundError(
                    f"Fichier OAuth introuvable: {CREDENTIALS_FILE}\n"
                    "Crée un client OAuth Desktop dans Google Cloud puis télécharge credentials.json."
                )

            logging.info("Authentification Google Drive requise...")
            flow = InstalledAppFlow.from_client_secrets_file(str(CREDENTIALS_FILE), SCOPES)

            # Compatible headless SSH:
            # ouvre une URL à copier/coller.
            try:
                creds = flow.run_console()
            except Exception:
                # fallback si run_console n'est pas dispo selon la version
                creds = flow.run_local_server(host="127.0.0.1", port=0, open_browser=False)

        TOKEN_FILE.write_text(creds.to_json())

    return build("drive", "v3", credentials=creds, cache_discovery=False)


def upload_file_to_drive(service, file_path: Path, drive_folder_id: str):
    metadata = {"name": file_path.name}
    if drive_folder_id and drive_folder_id != "PUT_YOUR_DRIVE_FOLDER_ID_HERE":
        metadata["parents"] = [drive_folder_id]

    media = MediaFileUpload(
        str(file_path),
        mimetype="image/jpeg",
        resumable=True,
    )

    result = service.files().create(
        body=metadata,
        media_body=media,
        fields="id, name"
    ).execute()

    return result


# =========================
# Camera
# =========================
def init_camera():
    picam2 = Picamera2()

    config = picam2.create_still_configuration(
        main={"size": IMAGE_SIZE},
        buffer_count=2,
    )
    picam2.configure(config)

    picam2.start()
    logging.info("Caméra démarrée. Stabilisation...")
    time.sleep(WARMUP_S)

    return picam2


def autofocus_before_capture(picam2: Picamera2):
    if not USE_AUTOFOCUS_CYCLE:
        return

    # Mode autofocus ponctuel avant chaque image
    picam2.set_controls({"AfMode": controls.AfModeEnum.Auto})
    try:
        ok = picam2.autofocus_cycle()
        logging.info("Autofocus cycle terminé. success=%s", ok)
    except Exception as e:
        logging.warning("Autofocus cycle indisponible ou en échec: %s", e)


def capture_one_image(picam2: Picamera2, destination: Path):
    autofocus_before_capture(picam2)
    metadata = picam2.capture_file(str(destination), format="jpeg")
    return metadata


# =========================
# Threads
# =========================
class CaptureThread(threading.Thread):
    def __init__(self, picam2, upload_queue: queue.Queue, stop_event: threading.Event):
        super().__init__(name="CaptureThread", daemon=True)
        self.picam2 = picam2
        self.upload_queue = upload_queue
        self.stop_event = stop_event

    def run(self):
        while not self.stop_event.is_set():
            loop_start = time.monotonic()

            ts = datetime.now().strftime("%Y%m%d_%H%M%S_%f")
            file_path = PENDING_DIR / f"img_{ts}.jpg"

            try:
                metadata = capture_one_image(self.picam2, file_path)
                logging.info("Photo capturée: %s | metadata=%s", file_path.name, metadata)
                self.upload_queue.put(file_path)
            except Exception as e:
                logging.exception("Échec capture: %s", e)

            elapsed = time.monotonic() - loop_start
            remaining = max(0.0, CAPTURE_INTERVAL_S - elapsed)
            self.stop_event.wait(remaining)


class UploadThread(threading.Thread):
    def __init__(self, upload_queue: queue.Queue, stop_event: threading.Event):
        super().__init__(name="UploadThread", daemon=True)
        self.upload_queue = upload_queue
        self.stop_event = stop_event
        self.service = None

    def ensure_service(self):
        if self.service is None:
            self.service = get_drive_service()

    def move_uploaded_file(self, file_path: Path):
        if not MOVE_AFTER_UPLOAD:
            return
        target = UPLOADED_DIR / file_path.name
        shutil.move(str(file_path), str(target))
        logging.info("Déplacé vers uploaded/: %s", target.name)

    def move_failed_file(self, file_path: Path):
        target = FAILED_DIR / file_path.name
        try:
            shutil.move(str(file_path), str(target))
            logging.info("Déplacé vers failed/: %s", target.name)
        except Exception as e:
            logging.warning("Impossible de déplacer le fichier en échec %s: %s", file_path, e)

    def run(self):
        while not self.stop_event.is_set():
            try:
                file_path = self.upload_queue.get(timeout=1.0)
            except queue.Empty:
                continue

            if file_path is None:
                self.upload_queue.task_done()
                continue

            if not Path(file_path).exists():
                logging.warning("Fichier absent, upload ignoré: %s", file_path)
                self.upload_queue.task_done()
                continue

            try:
                self.ensure_service()
                result = upload_file_to_drive(self.service, Path(file_path), DRIVE_FOLDER_ID)
                logging.info("Upload Drive OK: %s | id=%s", result.get("name"), result.get("id"))
                self.move_uploaded_file(Path(file_path))
            except Exception as e:
                logging.exception("Échec upload Drive pour %s: %s", file_path, e)

                # Si échec réseau/API, on requeue après pause.
                # Si tu préfères déplacer définitivement en failed/, remplace par self.move_failed_file(...)
                if Path(file_path).exists():
                    time.sleep(RETRY_DELAY_S)
                    self.upload_queue.put(Path(file_path))

            finally:
                self.upload_queue.task_done()


# =========================
# Main
# =========================
def preload_pending_files(upload_queue: queue.Queue):
    for file_path in sorted(PENDING_DIR.glob("*.jpg")):
        upload_queue.put(file_path)
        logging.info("Préchargé pour upload: %s", file_path.name)


def ensure_dirs():
    PENDING_DIR.mkdir(parents=True, exist_ok=True)
    UPLOADED_DIR.mkdir(parents=True, exist_ok=True)
    FAILED_DIR.mkdir(parents=True, exist_ok=True)


def main():
    setup_logging()
    ensure_dirs()

    logging.info("=== Démarrage capture + upload Google Drive ===")

    stop_event = threading.Event()
    upload_queue = queue.Queue(maxsize=200)

    def handle_signal(signum, frame):
        logging.info("Signal reçu (%s). Arrêt...", signum)
        stop_event.set()

    signal.signal(signal.SIGINT, handle_signal)
    signal.signal(signal.SIGTERM, handle_signal)

    preload_pending_files(upload_queue)

    picam2 = init_camera()

    # Un peu plus stable pour une caméra AF:
    try:
        picam2.set_controls({"AfMode": controls.AfModeEnum.Auto})
    except Exception as e:
        logging.warning("Impossible de régler AfMode au démarrage: %s", e)

    capture_thread = CaptureThread(picam2, upload_queue, stop_event)
    upload_thread = UploadThread(upload_queue, stop_event)

    upload_thread.start()
    capture_thread.start()

    try:
        while not stop_event.is_set():
            time.sleep(1)
    finally:
        stop_event.set()
        capture_thread.join(timeout=5)
        upload_thread.join(timeout=5)

        try:
            picam2.stop()
        except Exception:
            pass

        logging.info("=== Programme terminé ===")


if __name__ == "__main__":
    main()
