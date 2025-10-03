Raspberry Pi **CM5** (ve genel Linux/gpiochip) üzerinde **GPIO’ya doğrudan bağlı** basit bir keypad’i, sistem genelinde **klavye** gibi tanıtmak için hafif bir SDK + servis.
- GPIO olayları: **libgpiod**
- Sanal klavye: **uinput** → `Arrow/Enter/F-keys/...`
- Web, oyun, terminal: hepsi **gerçek klavye** gibi görür
- Uzun basış: varsayılan **Shift-on-Hold** (uzun basışta `Shift+…`)

- ####Kurulum için terminal komutu#######
```bash
bash <(curl -fsSL https://raw.githubusercontent.com/abdullahdogan/buttons-sdk/main/install.sh)


######Kaldırma için termial komutu #######
bash <(curl -fsSL https://raw.githubusercontent.com/abdullahdogan/buttons-sdk/main/uninstall.sh)

✨ Neler Sağlar?

Genel

GPIO → Klavye dönüştürücü: Her buton bir Linux KEY_* koduna eşlenir.
Uzun basış algılama: .hold_ms eşiği aşıldığında Shift-on-Hold ile Shift+Arrow gibi kombinasyonlar üretir (web’de e.shiftKey === true).
Debounce: Yazılımsal titreşim önleme (.debounce_ms) + OS auto-repeat (istersen özelleştirilebilir).
Daemon: systemd ile açılışta başlar, kapanırsa otomatik yeniden kalkar.

Esneklik
Eşleme listeleri: PINS[] (GPIO/aktif seviye/pull) ve KEYCODES[] (klavye kodları).
Hold işaretçisi (opsiyonel): Uzun basış başladığında bir defa KEY_F13 gibi “marker” gönderebilir.
C API (libbuttons.so): Uygulamana doğrudan buton olaylarıyla (PRESS/RELEASE/CLICK/HOLD/REPEAT) entegre ol.
Python: ctypes ile kütüphaneyi çağır.

Kullanım senaryoları
Web: Ekstra entegrasyon gerektirmez. keydown/keyup doğrudan çalışır.
Oyun/Emülatör: Oklar, Enter/Esc, A/Z gibi tuşlara eşle.
Kiosk: Yön + onay/iptal butonları ile menü kontrolü.
onanım & Bağlantı (CM5 odaklı)

Önerilen bağlantı (aktif-low)

3V3 ---[10k]---+                
               |
GPIO ----[buton]---- GND

Yazılımda: .active_low=true, .enable_pull=true (dahili pull-up talep eder).
Bazı CM5 taşıyıcı kartlarda kernel bias etkisiz olabilir → harici 10 kΩ pull-up önerilir.
3.3V sınırı: GPIO girişlerine 5V uygulamayın.
Kablo uzunsa veya EMI yüksekse:
RC debouncing: 10 kΩ + 100 nF (yaklaşık 1 ms)
Shield/GND referansı düzgün olsun; “yıldız topraklama” tercih edin.

Pin seçimi
I²C/SPI/UART gibi başka fonksiyonu olan pinlerden kaçının (ya da ilgili overlay’i kapatın).
BCM numarasıyla çalışıyoruz (fiziksel pin numarası değil).

BCM’i bulma & test:
pinout                                 # header ↔ BCM eşlemesi
gpiomon gpiochip0 17 27 22 5           # canlı RISING/FALLING gözle
“Resource busy” hatası varsa pin başka sürücü/overlay tarafından kullanılıyordur.


Pin & Tuş Eşleme (özelleştirme)
***Eşlemeyi examples/keypad-hid.c içinde yapın:

// 1) GPIO (BCM) listesi — kendi pinleriniz:
static const btn_pin_t PINS[] = {
  { .gpio=17, .active_low=true, .enable_pull=true }, // UP
  { .gpio=27, .active_low=true, .enable_pull=true }, // DOWN
  { .gpio=22, .active_low=true, .enable_pull=true }, // LEFT
  { .gpio=5,  .active_low=true, .enable_pull=true }, // RIGHT
  // { .gpio=6,  .active_low=true, .enable_pull=true }, // ENTER (örnek)
};

// 2) Bu pinlere karşılık gelecek klavye tuşları:
static const int KEYCODES[] = {
  KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT,
  // KEY_ENTER, KEY_SPACE, KEY_F13, ...
};

// 3) Davranış bayrakları:
static const bool ENABLE_SHIFT_ON_HOLD = true;  // uzun basışta SHIFT bas
static const bool ENABLE_HOLD_MARKER   = false; // true → hold başında KEY_F13 tap
static const int  HOLD_MARKER_KEY      = KEY_F13;

Zamanlamalar:
.debounce_ms = 12;    // titreşim önleme (ms)
.hold_ms     = 600;   // uzun basış eşiği (ms)
.repeat_ms   = 0;     // OS auto-repeat (EV_REP)

KEY_* listesi nerede?
grep -n 'define KEY_' /usr/include/linux/input-event-codes.h | less
man 7 input-event-codes

Değişiklikten sonra derle/kur/yeniden başlat:
cd ~/buttons-sdk
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local
cmake --build build -j"$(nproc)"
sudo cmake --install build
sudo systemctl restart keypad-hid


Hızlı Referans

Kur: bash <(curl -fsSL https://raw.githubusercontent.com/abdullahdogan/buttons-sdk/main/install.sh)
Kaldır:bash <(curl -fsSL https://raw.githubusercontent.com/abdullahdogan/buttons-sdk/main/uninstall.sh)
Servis: systemctl status|start|stop|restart keypad-hid
Eşleme: examples/keypad-hid.c → PINS[], KEYCODES[], hold_ms
Test: evtest → “Keypad HID (buttons-sdk)”
