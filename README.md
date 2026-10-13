# DeviceBoot

STM32U5A9J-DK için küçük custom bootloader. PC'deki [Device Studio](https://github.com/polite10/DeviceStudio), kartın mevcut ST-LINK Micro USB bağlantısı üzerinden firmware yükler. Bir uygulama alanı vardır: Smartwatch ve Düello sırayla yüklenir. Aynı anda saklanmazlar.

## Bellek yerleşimi

| Bölge | Adres | Boyut |
|---|---|---|
| Bootloader | `0x08000000` | 32 KiB |
| Yükleme commit kaydı | `0x08008000` | 8 KiB |
| Uygulama | `0x0800A000` | 4040 KiB |
| Saat kayıt günlüğü | `0x083FC000` | 16 KiB, güncelleyici dokunmaz |
| Reset isteği | `0x2026FFF0` | RAM'in son 16 baytı |

Bootloader vektörü `0x08000000`, uygulama vektörü `0x0800A000` adresindedir. Kimlik descriptor'ı `0x08007F00`, uygulama descriptor'ı uygulama başlangıcının `+0x400` adresindedir. Linker ve `SystemInit` bu düzenle eşleşmelidir. STM32U5A9xx, non-secure ve normal bank sırası hedeflenir; bank swap desteklenmez.

## Derleme

Bu repo kart SDK'sını kopyalamaz. Yanına [smart_watch](https://github.com/polite10/smart_watch) reposunu `Smartwatch` adıyla koyun. `sdk.lock.json` doğrulanmış commit'i belirtir. Windows / STM32CubeIDE 2.2.0 ARM GCC araçlarıyla:

```powershell
./Build.ps1
# Farklı konumdaki SDK:
./Build.ps1 -SmartwatchRoot C:/projects/Smartwatch
```

Çıktılar `Firmware/DeviceBoot.bin` ve `build/DeviceBoot.elf`. İlk kurulum Device Studio'daki **DeviceBoot kur** düğmesiyle ST-LINK/SWD üzerinden yapılır. Kurulum önce tam 4 MiB Flash yedeğini ve SHA-256 değerini yerel `Backup/` içine kaydeder. Ardından yalnız ilk beş Flash sayfasını (bootloader + commit) siler. Saat ayarları korunur. Uygulama, taşınmış `.dfw` paketinden ayrıca yüklenir. Eski `0x08000000` uygulaması bootloader kurulunca çalışamaz.

## Açılış ve kurtarma

- Normal açılışta vektörler, uygulama kimliği ve tam firmware CRC32'si doğrulanır. 900 ms bağlantı penceresinden sonra geçerli uygulama başlar.
- USER butonunu basılı tutarak resetlemek, uygulamanın `BOOT` komutu veya geçersiz/yarım firmware kurtarma modunu açar.
- Yükleme başlamadan commit sayfası silinir. Son yazılan veri doğrulanınca 16 baytlık commit kaydı en son programlanır.
- Güç/bağlantı kesilirse geçersiz uygulama başlatılmaz; paket baştan yüklenir. Önceki uygulamaya otomatik rollback yoktur.
- UART güncelleyici bootloader veya saat günlüğüne yazamaz. Bootloader'ın kendisi SWD ile güncellenir.
- Geri dönüş için ilk kurulumdaki tam Flash yedeğini SWD ile geri yükleyin. Yedek alındıktan sonraki saat kayıtlarını korumak için güncel son 16 KiB'ı ayrıca saklayın. Yedeğe dönüş custom bootloader'ı da kaldırır.

## Protokol v1

ST-LINK VCP ↔ USART1 PA9/PA10, bootloader **921600 8N1**, çalışan uygulama **115200 8N1**. Tek USB kablosu yeterlidir. İlk kurulum/teşhis SWD, sonraki uygulama yüklemeleri UART kullanır.

Paket başlığı little-endian: `DBT1` + sıra numarası (`u32`) + komut (`u32`) + payload boyutu (`u32`) + CRC32 (`u32`). CRC başlığın ilk 16 baytı ve payload üzerinden hesaplanır. En fazla 1040 payload baytı kabul edilir. Cevapta komutun bit 31'i set edilir; payload önce durum (`u32`), sonra JSON içerir.

| Komut | ID | Payload |
|---|---:|---|
| HELLO / STATUS | 1 / 6 | Yok |
| BEGIN | 2 | Boyut, görüntü CRC32, uygulama ID (`3 × u32`) |
| DATA | 3 | Offset (`u32`) + en fazla 1024 veri baytı |
| FINISH | 4 | Yok; CRC + vektör + kimlik doğrulanır |
| RUN | 5 | Yok; geçerli uygulama başlatılır |
| ABORT | 7 | Yok; geçersiz commit kurtarma modunda kalır |

Stop-and-wait: PC her bloğun ACK'ini bekler. Aynı sıra numarası ve paket CRC'sinin tekrarı son cevabı tekrar yollar; Flash'ı ikinci kez yazmaz. Son blok haricinde veri 16 bayt hizalıdır. Hata kodları: 1 komut/format, 2 boyut/kimlik, 3 Flash, 4 offset/oturum, 5 doğrulama, 6 başlatılamayan uygulama.

Bu geliştirme bootloader'ı bütünlük için CRC kullanır; dijital imza, şifreleme veya donanımsal yazma koruması uygulanmaz. UART üzerinden yükleme sınırları SWD erişimini kısıtlamaz.

Gerçek kart doğrulaması Device Studio reposundaki `hardware-report.json` içindedir. Test aracı `verify_hardware.py`, gerçek Flash içeriğini değiştirir ve sonunda Smartwatch'ı geri yükler.
