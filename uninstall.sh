#!/usr/bin/env bash
set -euo pipefail

SERVICE_NAME="${SERVICE_NAME:-keypad-hid}"
PREFIX="${PREFIX:-/usr/local}"

# 1) Servisi durdur/kapalı konuma al
echo "[*] Stopping & disabling service..."
sudo systemctl stop "$SERVICE_NAME" 2>/dev/null || true
sudo systemctl disable --now "$SERVICE_NAME" 2>/dev/null || true

# 2) Servis unit dosyasını sil ve daemon'u yenile
echo "[*] Removing systemd unit..."
sudo rm -f "/etc/systemd/system/${SERVICE_NAME}.service"
sudo systemctl daemon-reload || true

# 3) Kurulu dosyaları kaldır (manifest varsa onu kullan; yoksa bilinen yollar)
MANIFEST=""
for c in \
  "/var/lib/${SERVICE_NAME}/install_manifest.txt" \
  "/tmp/buttons-sdk-build/build/install_manifest.txt" \
  "$(dirname "$0")/build/install_manifest.txt"
do
  if [ -f "$c" ]; then MANIFEST="$c"; break; fi
done

if [ -n "$MANIFEST" ]; then
  echo "[*] Removing files from manifest: $MANIFEST"
  sudo xargs -a "$MANIFEST" -r rm -vf || true
else
  echo "[!] install_manifest not found — removing known files..."
  sudo rm -vf \
    "${PREFIX}/bin/${SERVICE_NAME}" \
    "${PREFIX}/lib/libbuttons.so" \
    "${PREFIX}/include/buttons.h" \
    "${PREFIX}/lib/pkgconfig/buttons.pc" \
    || true
fi

# 4) Linker cache
echo "[*] Refreshing dynamic linker cache..."
sudo ldconfig || true

# 5) (Opsiyonel) uinput autoload ayarını da kaldırmak istersen:
if [ "${PURGE_UINPUT:-0}" = "1" ]; then
  echo "[*] Removing uinput autoload config..."
  sudo rm -f /etc/modules-load.d/uinput.conf
fi

echo "[✓] Uninstall complete."
