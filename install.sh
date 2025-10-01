#!/usr/bin/env bash
set -euo pipefail

# >>>> BU SATIRI kendi repo adresinle GÜNCELLE <<<<
REPO="${REPO:-https://github.com/abdullahdogan/buttons-sdk.git}"

PREFIX="${PREFIX:-/usr/local}"
BUILD_DIR="${BUILD_DIR:-/tmp/buttons-sdk-build}"
SERVICE_NAME="keypad-hid"

echo "[*] Bağımlılıklar (Debian/RPi OS)..."
sudo apt-get update -y
sudo apt-get install -y git cmake build-essential libgpiod-dev pkg-config

echo "[*] uinput modülü..."
if ! lsmod | grep -q '^uinput'; then
  sudo modprobe uinput || true
fi
echo uinput | sudo tee /etc/modules-load.d/uinput.conf >/dev/null

echo "[*] Repo klonlanıyor: $REPO"
rm -rf "$BUILD_DIR"
git clone --depth=1 "$REPO" "$BUILD_DIR"

echo "[*] Derleme..."
cmake -S "$BUILD_DIR" -B "$BUILD_DIR/build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX"
cmake --build "$BUILD_DIR/build" -j"$(nproc || echo 2)"

echo "[*] Kurulum..."
sudo cmake --install "$BUILD_DIR/build"

# systemd servis dosyası (yoksa oluştur)
SERVICE_PATH="/etc/systemd/system/${SERVICE_NAME}.service"
if [ ! -f "$SERVICE_PATH" ]; then
  echo "[*] systemd servisi yazılıyor..."
  sudo tee "$SERVICE_PATH" >/dev/null <<UNIT
[Unit]
Description=Keypad → Virtual Keyboard (uinput)
After=multi-user.target

[Service]
Type=simple
ExecStartPre=/sbin/modprobe uinput
ExecStart=${PREFIX}/bin/${SERVICE_NAME}
Restart=always
RestartSec=0.5

[Install]
WantedBy=multi-user.target
UNIT
fi

echo "[*] Servis etkinleştiriliyor..."
sudo systemctl daemon-reload
sudo systemctl enable --now "${SERVICE_NAME}"

echo
echo "✅ Kurulum tamam."
echo "• Servis durumu:    systemctl status ${SERVICE_NAME} --no-pager"
echo "• Canlı log:        journalctl -u ${SERVICE_NAME} -f"
echo "• Harita değiştirme: ${PREFIX}/bin/${SERVICE_NAME} kaynağı: examples/keypad-hid.c içindeki PINS[]/KEYCODES[]"
echo "• Güncelleme:       kodu değiştir → yeniden çalıştır:  sudo systemctl restart ${SERVICE_NAME}"
