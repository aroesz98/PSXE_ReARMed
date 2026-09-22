# PSX GPU board – STM32H7S78-DK / Zephyr RTOS

Druga płytka emulatora PSX: **rasteryzacja i wyświetlanie klatek** dla
`psxe_embedded` na i.MX RT1050. RT1050 emuluje CPU, JIT, CD, SPU, DMA i zegary
GPU, a każde słowo GP0/GP1 wysyła Ethernetem tutaj; ta płytka trzyma VRAM
(1 MB w PSRAM), rasteryzuje **tym samym `PSXE/dev/gpu.c`** i pokazuje obraz
na swoim panelu 800×480 (skalowanie przez NeoChrom, flip przez LTDC).

| | |
|---|---|
| Płytka | STM32H7S78-DK (STM32H7S7L8, Cortex-M7 600 MHz, LCD 800×480, LAN8742A) |
| Zephyr | 4.4 (`stm32h7s78_dk/stm32h7s7xx/ext_flash_app`, sysbuild + MCUboot) |
| Łącze | Ethernet 100 Mbit, surowe ramki (EtherType `0x88B5`), bez stosu IP |
| Druga strona | `PSXE/link/gpu_remote.c` + `source/enet_link.c` w projekcie RT1050 (`PSXE_GPU_REMOTE`) |

## Podłączenie

1. Kabel Ethernet (zwykły, oba PHY mają auto-MDIX) między **J19** na EVKB
   a gniazdem RJ45 na DK.
2. Na DK zworka **JP6 w pozycji PC1** (RMII dla PHY).
3. Obie płytki na USB (ST-LINK VCP na DK = konsola 115200 8N1, na EVKB COM
   debug jak dotąd).

Kolejność włączania nie ma znaczenia: DK czeka na link i na pierwsze ramki,
RT1050 wysyła dopiero gdy DK odpowie `STATUS` z flagą READY, a przy każdym
(ponownym) zestawieniu łącza odtwarza stan wyświetlania (GP1) i środowisko
rysowania (GP0 E1–E6). Tekstur wgranych do VRAM przed zestawieniem łącza nie
da się odtworzyć – po podpięciu kabla w trakcie gry obraz jest poprawny dopiero
gdy gra sama je wgra ponownie (zmiana sceny), najprościej zresetować RT1050.

## Budowanie i wgrywanie

```powershell
.\build.ps1 -Flash          # MCUboot + aplikacja (pierwszy raz)
.\build.ps1 -FlashApp       # później tylko aplikacja
.\build.ps1 -Pristine       # czysty build
```

Po stronie RT1050: `build.bat -Flash` (tryb zdalnego GPU jest domyślny,
`PSXE\gpu_switch.h`; `build.bat -Define PSXE_GPU_REMOTE=0` wraca do lokalnego
rasteryzera i LCD EVKB).

## Protokół (`PSXE/link/psxe_link.h`)

Ramka: nagłówek Ethernet, 4 bajty (wersja, flagi, numer sekwencyjny), potem
rekordy 32-bitowe `[typ:4 | flagi:4 | długość w słowach:24][słowa…]`:

| Rekord | Kierunek | Treść |
|---|---|---|
| `GP0` | RT1050 → DK | słowa GP0 (flaga BOUNDARY: rekord zaczyna się od słowa komendy) |
| `GP1` | RT1050 → DK | jedno słowo GP1 |
| `VBLANK` | RT1050 → DK | koniec klatki: pole przeplotu, numer klatki – DK prezentuje |
| `RESET` | RT1050 → DK | `psx_gpu_init` |
| `STATUS` | DK → RT1050 | zużyte bajty (kredyt), flagi READY/RESYNC, rozmiar ringu, klatki pokazane, błędy, boot id |
| `VRAM` | DK → RT1050 | dane transferu VRAM→CPU (GP0 C0h), tak jak zwracałby je GPUREAD |

Sterowanie przepływem: RT1050 ma w locie najwyżej `PSXE_LINK_WINDOW`
(448 KB) bajtów ponad to, co DK zgłosiło jako zużyte; ring odbiorczy DK ma
512 KB. Gdy okno się wyczerpie, emulacja czeka (to jedyny nacisk wsteczny).
Utrata ramki (luka w numerach) przełącza DK w RESYNC: pomija słowa GP0 do
rekordu z flagą BOUNDARY.

## Co działa gdzie

* **RT1050** (`gpu_remote.c`): shadow-parser GP0 – długości komend, granice,
  rozmiary transferów A0h/C0h, bity texpage/E1 w GPUSTAT (gra je czyta),
  bufor odczytu VRAM→CPU (16 K słów), kredyt, replay po zestawieniu łącza.
  `gpu.c` w trybie zdalnym nie parsuje ani nie rysuje; nadal liczy hblank/vblank,
  GPUSTAT.31, pola przeplotu i stan GP1 (`GP1(10h)` odpowiada lokalnie).
* **DK** (`src/main.c`): wątek RX → ring; pętla główna: rekordy → `gpu.c`,
  `VBLANK` → `psx_gpu_set_field` + prezentacja, `STATUS` co ≤2 ms po zmianie
  albo co 16 KB, heartbeat co 250 ms.
* **Prezentacja** (`src/present.c`): okno wyświetlania z VRAM (15 bpp albo
  24 bpp) przepakowane przez CPU do RGB565 (tylko wiersze, które GPU zmieniło –
  te same liczniki brudnych wierszy co na RT1050), potem NeoChrom skaluje do
  640×480 (point-sampling przy całkowitej skali, inaczej bilinear), trzy bufory
  ramki w PSRAM, flip przez rejestr cienia LTDC. Gry rysujące jedno pole na
  klatkę (Tekken 3) pokazywane są polami.
* **Konsola DK** co 5 s: liczniki ramek/bajtów, słowa GP0, vblanki, straty,
  resynci, zajętość ringu, klatki pokazane, czas repack/GPU.

## Pamięć (DK)

| | |
|---|---|
| AXI SRAM | hot code `gpu.c` (`.ramfunc`, 128 KB), bufory ETH i pula NemaGFX (`__nocache`), stosy |
| DTCM | stan `psx_gpu_t` i bufor CLUT (`psx_gpu_platform.h`) |
| PSRAM | VRAM 1 MB, ring 512 KB, staging 640×480 RGB565, 3 × bufor ramki 800×480 |

## Dalej (faza B)

Rysowanie prymitywów przez NeoChrom zamiast `gpu.c`: tekstury LUT4/LUT8 z
palety (`nema_bind_lut_tex`), tryby przezroczystości przez blend/alfa palety,
gouraud + tekstura w dwóch przejściach; fallback programowy dla reszty.
Uwagi o sprzęcie (nibble swap LUT8, brak ditheru, brak Z) – w
`../h7s7_neochrom/README.md`.
