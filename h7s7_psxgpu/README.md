# PSX GPU board – STM32H7S78-DK / Zephyr RTOS

Druga płytka emulatora PSX: **wyświetlanie i skalowanie klatek** dla
`psxe_embedded` na i.MX RT1050, przez akcelerator **NeoChrom (GPU2D)**.

Tryb wybiera się **sam**: gdy DK odpowie przez Ethernet, RT1050 przełącza się na
hybrydę i obraz idzie na panel 800×480 tej płytki; gdy DK nie ma (brak kabla,
reset, wyłączona), RT1050 rysuje po staremu na własnym LCD przez PXP. Przejście
w obie strony dzieje się w locie, bez restartu gry.

| | |
|---|---|
| Płytka | STM32H7S78-DK (STM32H7S7L8, Cortex-M7 600 MHz, LCD 800×480, LAN8742A) |
| Zephyr | 4.4 (`stm32h7s78_dk/stm32h7s7xx/ext_flash_app`, sysbuild + MCUboot) |
| Łącze | Ethernet 100 Mbit, surowe ramki (EtherType `0x88B5`), bez stosu IP |
| Druga strona | `PSXE/link/gpu_remote.c` + `source/enet_link.c` w projekcie RT1050 |

## Podział pracy

* **RT1050**: emulacja CPU/GTE/CD/SPU/DMA i zegary GPU. Strumień GP0/GP1 idzie
  na DK (`PSXE_GPU_REMOTE_CMD=1`, domyślnie w wariancie strumieniowym).
* **DK**: VRAM, **rasteryzacja na NeoChromie** (`src/nema_raster.c`) i
  wyświetlanie. `gpu.c` jest tu tylko parserem komend i stanu — każdy prymityw,
  który normalnie by narysował, wychodzi przez szew `PSX_GPU_EXTERNAL_RASTER`
  do NeoChroma.

Zmierzone na tej płytce, jeden trójkąt 24 px (CPU na budowę komendy + GPU):

| | NeoChrom | rasteryzer programowy |
|---|---|---|
| płaski | 0,36 + 0,21 µs | ~2 µs |
| gouraud | 2,04 + 0,15 µs | — |
| teksturowany LUT4 | 1,36 + 0,14 µs | 12,5 µs |
| teksturowany gouraud | jw. | 30 µs |

**VRAM na DK jest w RGBA5551**, nie w formacie PSX: GPU2D nie zapisuje żadnego
innego formatu 16-bitowego (sonda przy starcie to wypisuje). Konwersja jest
tylko na granicach — wgranie tekstury (GP0 A0h) i odczyt do CPU (C0h) — a bit
alfa jest 1 dla każdego piksela różnego od zera, czyli dokładnie zasada PSX
„teksel 0000h jest przezroczysty". Dzięki temu prezentacja nie wymaga już
przepakowania: panel karmiony jest wprost z VRAM.

### Co jest przybliżone

* **przezroczystość jest na prymityw, nie na teksel** — PSX bierze ją z górnego
  bitu każdego teksela, czego nie zrobi żaden blender;
* **tryb 2 (B − F)** nie ma odpowiednika (brak blendu odejmującego) i jest
  rysowany jako B × (1 − F), co przyciemnia podobnie; pozostałe trzy tryby są
  dokładne;
* **teksturowany gouraud** moduluje średnią z trzech kolorów wierzchołków —
  jednostka gradientu służy wypełnieniom; przy małych trójkątach nie widać;
* **okno tekstury (GP0 E2h)** jest ignorowane — sampler nie ma takiej maski;
* **gry rysujące jedno pole przeplotu na klatkę** są rysowane w całości: GPU nie
  umie pomijać wierszy, a przy 0,14 µs na trójkąt drugie pole jest tańsze niż
  zachód. Obraz wychodzi progresywny, bez grzebienia.

Statystyki na konsoli DK co 5 s: linia `raster:` z liczbą prymitywów po
klasach, przywiązaniami tekstur, paletami i czasem spędzonym w GPU.

## Podłączenie

1. Kabel Ethernet (zwykły, oba PHY mają auto-MDIX) między **J19** na EVKB
   a gniazdem RJ45 na DK.
2. Na DK zworka **JP6 w pozycji PC1** (RMII dla PHY).
3. Obie płytki na USB (ST-LINK VCP na DK = konsola 115200 8N1, na EVKB COM
   debug jak dotąd).

Kolejność włączania nie ma znaczenia i kabel można wpiąć w trakcie gry: DK
czeka na `STATUS`, a RT1050 po jego otrzymaniu wysyła najpierw **całą** klatkę
(flaga WHOLE), więc obraz jest poprawny od razu. Wyjęcie kabla albo reset DK
przełącza obraz z powrotem na LCD EVKB w tej samej klatce.

W trybie strumienia GP0 (`PSXE_GPU_REMOTE_CMD=1`) jest inaczej: RT1050 odtwarza
wtedy stan wyświetlania, środowisko rysowania i cały 1 MB VRAM z własnego cienia.

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
| `FRAME` | RT1050 → DK | **hybryda**: wiersze gotowej klatki z VRAM (nagłówek: szerokość/wysokość, pierwszy wiersz, liczba wierszy, flagi 24bpp/blank/last/whole, GP1(08)) |
| `GP0` | RT1050 → DK | tryb strumienia: słowa GP0 (flaga BOUNDARY: rekord zaczyna się od słowa komendy) |
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

## Co działa gdzie (tryb strumienia GP0)

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

## Pomiary sprzętowe (2026-09-22)

Wypisywane przy starcie DK, do powtórzenia po każdej zmianie:

* `memory:` PSRAM 189 cykli na zimną linię, SRAM 6; 256 KB sekwencyjnie:
  odczyt 1,06 ms, zapis+flush 0,74 ms.
* `GPU primitives, 2000 of 24px`: flat 14,5 ms, gouraud 15,1 ms, LUT4 37,0 ms
  na 2000 trójkątów (7,3 / 7,6 / 18,6 µs na sztukę) – stąd wniosek, że NeoChrom
  nie zastąpi rasteryzera dla typowej sceny PS1 (tysiące małych trójkątów).
* sonda formatów: `RGB565 -> RGB565 (control)` poprawny, wszystkie warianty
  1555 zwracają zera przy odczycie, a przy zapisie tylko `RGBA5551` daje
  sensowne piksele.

Uwagi o sprzęcie NeoChroma (nibble swap LUT8, brak ditheru, brak Z) – w
`../h7s7_neochrom/README.md`.
