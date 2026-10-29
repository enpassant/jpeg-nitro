<p align="center"><img src="docs/logo.webp" alt="NitroView – fast image viewer for macOS, Windows and Linux" width="800"></p>

# jpeg-nitro

[English](README.md) | **Magyar**

**nitroview**: nagyon gyors képnéző **macOS-re (Intel és Apple Silicon), Windowsra és GNOME-os
Linuxra**:

| platform | néző | fordítás |
|---|---|---|
| macOS 12-től, Intel és Apple Silicon | Cocoa + Metal: universal `nitroview.app` | `make app` / `make dmg` |
| Windows 10-től | Win32 + Direct3D 11: egyetlen önálló `nitroview.exe` | `make windows` (llvm-mingw) |
| Linux, GNOME | GTK 4 + libadwaita: `nitroview-gnome`, Flatpakként is | `make gnome` / `make flatpak` |
| Linux (Wayland / X11), új | SDL3 + OpenGL 3.3: egyetlen önálló `nitroview-sdl` | `make sdl` / `make sdl-dist` |

Kész buildek mind a négyhez: [Releases](https://github.com/gereoffy/jpeg-nitro/releases). Az ottani macOS
dmg **hitelesített (notarized)**: Gatekeeper-figyelmeztetés nélkül nyílik.

Mind a négy ugyanazokra a saját dekóderekre épül: hármat, amelyek egyetlen képet is az összes CPU-magon
dekódolnak — pedig a JPEG, a PNG és a PSD tömörítése is eredendően soros —, egyet, amely a HEIC-fotók
csempéit párhuzamosan dekódolja, egyet a DNG nyers képek előhívásához, egyet-egyet a WebP-hez, a TIFF-hez
és az AVIF-hez (ez utóbbi a dav1d-vel), és kettőt a vektoros rajzokhoz:

| dekóder | formátum | hogyan | idő | szokásos dekóderek |
|---|---|---|---|---|
| **nitrojpeg** | baseline és progresszív JPEG | spekulatív párhuzamos Huffman-dekódolás, AVX2 IDCT; progresszív: a menetek csővezetékben, soronként; bitre egyezik a libjpeg-turbóval | 24 MP-es fotó: **~11 ms**; progresszív: **~79 ms** | libjpeg-turbo ~107 ms (1 szálon); progresszív ~336 ms |
| **nitropng** | PNG (nem interlaced; palettás, 1–16 bites, átlátszósági kulccsal is) | spekulatív párhuzamos inflate (a még ismeretlen adatra mutató visszahivatkozásokat utólag oldja fel, Adler-32-vel ellenőrizve) + „hullámfront” szűrővisszafejtés | 38 valódi PNG: átlag **~43 ms** | Wuffs ~171 ms, ImageIO ~318 ms |
| **nitropsd** | Photoshop PSD/PSB (8 bites RGB / szürke) | összesített kép: RLE-sorok párhuzamosan, ZIP a nitropng inflate-jével; nagy, rétegekkel teli fájlból csak az összesített képet olvassa be | 24 MP RLE: **~5 ms** | ImageIO ~170 ms |
| **nitroheic** (macOS) | HEIC (8 bites 4:2:0 HEVC, iPhone-rácsok) | saját HEIF-feldolgozás; egy iPhone-fotó 48 független HEVC-csempéjét a VideoToolbox párhuzamosan bontja ki Y/Cb/Cr síkokba a GPU-nak | 12 MP-es iPhone-fotó: **~33 ms** | ImageIO ~200 ms |
| **nitrotiff** | TIFF (tömörítetlen, PackBits, LZW, Deflate, CCITT G3 / G4; 1–16 bites szürke, palettás, RGB, CMYK, alfa; csíkok / csempék, chunky / planar) | a csíkok és csempék párhuzamosan (a Deflate a nitropng párhuzamos inflate-jével is); tömörítetlen: közvetlenül a fájlból alakítja át, párhuzamosan | 13 MP-es 16 bites szken (73 MB): **~2 ms**; 17 MP LZW: **~6 ms** | ImageIO ~31 ms; ~46 ms |
| **nitroavif** | AVIF (AV1 HEIF-ben: 8–12 bit, 4:2:0 – 4:4:4, szürke, alfa, rácsok, vágás / forgatás / tükrözés) | saját HEIF-feldolgozás (közös a nitroheic-kel), az AV1-et a [dav1d](https://code.videolan.org/videolan/dav1d) bontja ki; a rács csempéi párhuzamosan, saját párhuzamos színkonverzió (a chroma lineárisan nagyítva) | 41 webes AVIF: átlag **~10 ms** (M1: 10,8 ms; Windows, i5: 13 ms) | ImageIO ~32 ms (M1: 22 ms); WIC: csak Store-bővítménnyel |
| **nitrosvg** | SVG (szűrők nélkül) | lunasvg-értelmező + plutovg-renderelő, fontkereséssel a rendszer és a felhasználó fontkönyvtáraiban | 38 MB-os SVG: **~0,53 s** | WebKit ~1,4 s, CoreSVG ~2,4 s |
| **nitrocdr** | CorelDRAW 5 – X6 (CDR), CMX | saját [libcdr-forkunk](https://github.com/gereoffy/libcdr) sok javítással (szövegstílusok, sorköz, vágás, bitkép-átlátszóság) és kétszer gyorsabb librevenge-dzsel; közvetlenül plutovg-vel rajzol | 17,7 MB-os, csak vektoros CDR: **~1,8 s**, ~3× gyorsabb | eredeti libcdr `cdr2xhtml` ~5,4 s (csak SVG-vé alakítás) |

Az idők Intel i9-en mérve. A dekóderek ARM-on (NEON) is gyorsak: egy 2020-as M1-es MacBook
Airen egy 24 MP-es JPEG ~30 ms alatt dekódolódik, ~3×-osan gyorsabban, mint ugyanazon a gépen az
Apple hardveres JPEG-dekódere (lásd lent: „Apple Silicon (M1)”). A dekóderek függőség nélküli
C-fájlok, önállóan is használhatók. A fotósorozatok (pl.
timelapse) teljes felbontásban, valós időben „lejátszhatók”, akár 60 kép/s-mal. macOS-en és Windowson a **Canon
CR2** és **Nikon NEF** raw fájlokból a gép által beléjük tett teljes méretű JPEG jelenik meg (a nitrojpeggel, néhány
ms alatt az ImageIO 150–400 ms-a helyett; az elforgatásukat is figyelembe veszi). macOS-en a **HEIC**
fotókat (iPhone) a nitroheic (`src/nitroheic.c`) dekódolja: egy iPhone-fotó 48 független HEVC-csempéből
álló rács, ezeket párhuzamosan bontja ki Y/Cb/Cr síkokba (Intelen magonként egy szoftveres
VideoToolbox-dekóderrel, Apple Siliconon 4 szálon a médiamotorral), a színkonverzió a GPU-n megy — egy
12 MP-es kép ~33 ms alatt az ImageIO ~200 ms-a helyett egy Intel i9-en (M1: lásd lent: „Apple Silicon
(M1)”).
A **TIFF** képeket a nitrotiff (`src/nitrotiff.c`, lásd lent: „TIFF (nitrotiff)”) bontja ki. A JPEG-tömörítésű
TIFF, a WebP, GIF, BMP és a többi HEIC macOS-en az Apple ImageIO-val, Windowson a WIC-kel, Linuxon a GTK betöltőivel nyílik
meg (lásd lent: „Windows” és „GNOME / Linux”). macOS-en és Windowson vektoros rajzokat is mutat: **SVG**-t
(lunasvg) és **CorelDRAW** CDR / CMX fájlokat (libcdr, a saját, sok javítást tartalmazó forkunk),
bármekkora nagyításban élesen, és ugyanolyan gyors lapozással, mint a fotóknál (lásd lent: „SVG” és
„CorelDRAW”); a GNOME-os változat az SVG-t a GTK-val jeleníti meg.
A név utalás: versenyautókban a turbót nitróval gyorsítják tovább.

Intel x86-64 Macen (i9, 13. gen., 8 mag / 16 szál, AMD RX 580) fejlesztve és mérve.
Apple Siliconon is fut (M1-es MacBook Air, lásd lent); ott az IDCT az AVX2 helyett NEON-t használ.

```
./nitroview [-f] [-s ms] [-j szálak] kep1.jpg kep2.jpg ... | konyvtar/
```

| billentyű | funkció |
|---|---|
| PgDn / Space | következő kép |
| PgUp / Backspace | előző kép |
| Home / End | első / utolsó |
| [ / ] | többoldalas CorelDRAW-rajz előző / következő oldala (címsor: [page 2/5]) |
| + / − | nagyítás / kicsinyítés (√2-es lépések, pontosan megáll 100%-nál és az illesztett méretnél) |
| 0 | illesztés a képernyőhöz: a kis képeket is felnagyítja (lapozáskor is megmarad, a W-ig) |
| 1 | eredeti méret: egy képpixel = egy képernyőpixel |
| 2 … 8 | 200% … 800%, pixelpontosan: egy képpixel = N×N képernyőpixel |
| ← → ↑ ↓ | a nagyított kép mozgatása (Shifttel fél képernyőnyi lépés) |
| W | vissza a kép saját méretére (kis képek 100%-on), az ablak újra követi a képek méretét |
| egérhúzás | a nagyított kép mozgatása |
| görgő | nagyítás a kurzor alatti pont körül (kattanásonként √2-es lépés; trackpaden folyamatos, csíptetés is) |
| dupla kattintás | illesztett → 100% a kattintott ponton; újra → vissza illesztettre |
| jobb kattintás / Shift + jobb kattintás | következő / előző kép |
| Ctrl + görgő | előző (előre görgetve) / következő (hátra görgetve) kép; trackpaden 40 képpontonként egy kép |
| egér oldalgombok (vissza / előre) | előző / következő kép |
| F / Enter | teljes képernyő be/ki (`-f`: indításkor teljes képernyő) |
| P | diavetítés szüneteltetése / folytatása |
| Esc / Q | kilépés (Esc teljes képernyőn: vissza ablakba) |

Az ablakcím mutatja a fájlnevet, a méretet és a dekódolási időt. A stdout-ra
minden képnél kiíródik: olvasás, dekódolás, és a billentyű → képernyő idő.
Az EXIF-orientációt figyelembe veszi.

**Diavetítés:** `-s <ms>` (vagy `--slideshow <ms>`): a következő kép akkor jön, amikor az
aktuális már `ms` ezredmásodperce a képernyőn van (pl. `-s 2000` klasszikus diavetítés,
`-s 40` ~25 kép/s). Ha egy kép betöltése tovább tart, kivárja, így egy kép sem marad ki.
Az utolsó képnél megáll. P szünetelteti, a lapozó gombok közben is működnek, és onnan
folytatja. Az időzítés a monitor frissítéséhez igazodik (60 Hz-en ±8 ms).
Paraméterek nélkül (vagy `-h`) a program kiírja az összes kapcsolót.

**Más formátumok:** a JPEG mellett a nitroview megnyitja a PNG, PSD/PSB, HEIC/HEIF, TIFF, WebP, GIF
és BMP fájlokat is. PSD/PSB-nél az összefésült (kompozit) kép jelenik meg: 8 bites RGB és
szürkeárnyalatos fájloknál a **nitropsd** dekódolja (mind a négy tömörítéssel: nyers, RLE, ZIP, ZIP
predikcióval), egyébként az Apple ImageIO – ami a ZIP-es PSD-ket egyáltalán nem tudja megnyitni. Egy
24 MP-es RLE-s PSD ~5 ms (ImageIO: ~170 ms), a ZIP-es ~46 ms: az RLE sorhossz-táblájából minden sor
helye kiszámolható, így a sorok párhuzamosan bonthatók ki; a ZIP a nitropng párhuzamos inflate-jét
használja. A nagy, réteges PSD-k nagy része rétegadat (az összefésült kép sokszor a fájlnak csak
13–23%-a), ezért a néző csak a fejlécet, az erőforrásokat (ICC-profil) és az összefésült képet olvassa
be: egy 482 MB-os PSD beolvasása ~155 ms helyett ~20 ms.
Átlátszó összefésült kép (negatív rétegszám): a Photoshop a színt fehér háttérre keverve tárolja – egy
tesztfájlban minden részben átlátszó pixel minden csatornája ≥ 255 − alfa volt –, ezért a nitropsd
fekete háttéren max(0, szín + alfa − 255)-ként mutatja. Az ImageIO ezt a fehér keverést nem veszi le
(világos szegély a lágy széleken). Pozitív rétegszámnál a 4. csatorna mentett kijelölés, nem
átlátszóság. A „Maximize Compatibility” nélkül mentett fájlokban az összefésült kép helyén csak fehér
helykitöltő van; a rétegeket a néző nem fésüli össze, így ezek fehéren jelennek meg (mint az
ImageIO-ban). 55 valódi PSD-n (Photoshop-munkák, térképek, építészet) az eredmény e két ponttól
eltekintve egyezik az ImageIO-val. A PNG-t a **nitropng**, a saját párhuzamos PNG-dekóderünk
dekódolja (nem interlaced: a 8 bites szürke / RGB / RGBA úgy, ahogy van, a GPU-ra megy; a palettás,
az 1 / 2 / 4 / 16 bites és az átlátszósági kulcsos PNG-ket párhuzamosan BGRX-re alakítja), az
ritka interlaced képeket az Apple ImageIO, mint a többi formátumot (`make WUFFS=1`-gyel előbb a
[Wuffs](https://github.com/google/wuffs)). Az átlátszó képek fekete háttéren jelennek meg. 38 valódi PNG-n
(képernyőképek, szkennelések, AI-képek, felskálázott textúrák, Photoshop-exportok) a nitropng átlag
**~43 ms/kép**, a Wuffs ~171, az ImageIO ~318 ms (lásd lent: „nitropng”). 25 palettás / 1–16 bites /
átlátszósági kulcsos PNG-n a néző dekódolása 53 ms-ról (Wuffs) 18 ms/képre gyorsult, pixelre azonosan
az ImageIO-val (`bench/bgrxverify`; egy 26 MP-es 16 bites RGB ~940-ről ~195 ms-ra).

**SVG** (macOS-en és Windowson, ha a `scripts/get-deps.sh` letöltötte a [lunasvg](https://github.com/sammycage/lunasvg)-t):
a lunasvg rajzolja fehér háttérre, először egészben, a hosszabbik oldalt 1024–4096 pixelre
igazítva (a kis ikonok nagyobbak lesznek, a hatalmas rajzok kisebbek; teljes képernyőn vagy a 0
billentyű után legalább képernyőméretben, az előre betöltött képek is, így rögtön élesek), így a lapozás, az
ablakhoz igazítás és a kicsinyítés ugyanúgy megy, mint a fotóknál. Ennél jobban felnagyítva a
látható részt képernyő-felbontásban újra kirajzolja, amikor a nagyítás vagy a pozíció ~80 ms-ig
nem változik (jellemzően 5–40 ms alatt); addig az előző rajzolat nagyul, így a csippentés
folyamatos marad, és a kép a megállás után élesedik ki. Amit ismer: alakzatok, útvonalak,
színátmenetek, vágás, maszk, minta, `<style>` stíluslap, szöveg (a rendszer betűtípusaival),
beágyazott képek; a nitrosvg betöltés előtt három dolgot javít, amit a lunasvg rosszul kezelne:
a `svg:` előtagos elemeket (LibreOffice), a CSS `var(--x, tartalék)` színeket (a VS Code képei: a tartalékszín, ha nincs, az örökölt érték;
különben fekete négyzetek) és
az SVG 1.1 szerinti sortöréseket a szövegben (régi exportálók sorokra tördelik a szavakat). A
`patches/lunasvg.patch` (a `scripts/get-deps.sh` alkalmazza) magát a lunasvg-t javítja: az ismételt
lezárás (`ZZ`) után az útvonal többi része elveszett, nagy nagyításnál pedig a maszkok az egész elem
méretű rétegeket foglaltak (összeomlás). Amit nem: szűrők (az árnyék és az elmosás kimarad), HTML-feliratok
`<foreignObject>`-ben (draw.io-ábrák), SVG-betűtípusok és a csak szimbólumokat tartalmazó
sprite-fájlok, amelyek nem rajzolnak semmit. Egy Macről összegyűjtött 883 SVG-ből (ikonok, logók,
ábrák, Illustrator-exportok) 867 nyílik meg; a többi 16 böngészőben sem rajzol semmit.

**CorelDRAW** (CDR és CMX, macOS-en és Windowson, ha a `scripts/get-deps.sh` letöltötte a libcdr-t): a libcdr
(a LibreOffice-é, az Inkscape és a LibreOffice Draw is ezt használja) olvassa be a rajzot; a nitrocdr
(`src/nitrocdr.cpp`) rajzlistába gyűjti, amit átad, és közvetlenül a plutovg-vel rajzolja ki, SVG
nélkül, így a beágyazott szkennelések és fotók bitképként maradnak a memóriában. A megjelenített
terület a rajz befoglaló kerete (kis margóval), nem a fájlban beállított lapméret, így egy kis logó
is kitölti az ablakot. A lapozás, a nagyítás és az éles részlet úgy megy, mint az SVG-nél
(többoldalas dokumentum oldalai között a [ és ] vált). A libcdr a saját forkunkból jön
([gereoffy/libcdr](https://github.com/gereoffy/libcdr)), benne a szükséges javítások, mindegyik
külön commit: a nyitott vonalak nincsenek kitöltve; a szöveg megtartja a forgatását, a vízszintes
nyújtását, a félkövér és dőlt stílusát, a pontos méretét és az importált PDF-szöveg szavait elhelyező
eltolásokat, és a valódi kezdőpontjára kerül; a `CMYK100` színek (CorelDRAW 2022)
nem fehérek; a rejtett rétegek elemei kimaradnak (egy importált PDF a kikapcsolt rétegeit rejtett
rétegként tartja meg); a szimbólumdefiníciók nem adnak egy plusz oldalt; a bitképek megtartják az
átlátszó hátterüket, és a saját körvonalukra vágva jelennek meg (az importált tervek falsraffozása); a CorelDRAW 5 és 7–X5 fájlok elveszett szövegei megjelennek; és az ICU helyett
iconv dolgozik (kódlapok; karakterkészlet nélkül windows-1250, ha a szövegben ő/ű van). A
`patches/librevenge.patch` a beágyazott képeket base64 nélkül engedi át. A fontokat a /Library/Fonts,
a /System/Library/Fonts és a ~/Library/Fonts mappából veszi (a Bitstream-nevek, pl. „Futura Lt
BT”, a családra képződnek le); hiányzó font helyett hasonlót használ, ilyenkor a szöveg kicsit
hosszabb vagy rövidebb lehet. A vonalak legalább egy képpont szélesek, mint a CorelDRAW-ban. Amit nem tud: nyílhegyek, a Corel saját vektoros és
bitképes mintakitöltései. Egy Macről összegyűjtött 216 CDR (ArchiCAD-PDF-ből készült alaprajzok,
hálózati térképek, logók) mind megnyílik; egy Intel i9-en a medián 20 ms, a 76 darab 1 MB-nál nagyobb
fájl átlagosan 230 ms, a legnagyobb tervrajz 1,8 s (nagyrészt a libcdr feldolgozása). macOS-en és a Linuxos SDL-nézőben a C++ kód (libcdr,
librevenge, lunasvg) a [mimalloc](https://github.com/microsoft/mimalloc)-kal foglal memóriát: egy nagy
terv betöltése több millió apró foglalással jár, és így Intelen és Apple Siliconon is 20–25%-kal
gyorsabban töltődnek be a tervek (`make MIMALLOC=0`: a rendszer malloc-ja); 193 CDR az M1-en Linux
alatt 16%-kal gyorsabb, mint a glibc malloc-jával.

**Színkezelés:** a beágyazott ICC-profilokat minden formátumnál figyelembe veszi (pl. Display P3 az
iPhone-okról és a Mac-es képernyőképekből, Adobe RGB a szkennelésekből); a profil nélküli képeket
sRGB-nek tekinti. A dekódolt képpontok változatlanok maradnak, a kirajzoló réteg kapja meg a kép
színterét, így a monitor profiljára a macOS számol át, többletköltség nélkül, és a széles
színtartomány is megmarad.

A `-j N` a dekóder szálainak számát korlátozza (alapból mind a 16 logikai szál). 8 szálon
a dekódolás ~15.4 ms/kép a 11.8 helyett, de kevésbé melegíti a CPU-t.

**Egyetlen fájllal indítva** (pl. Midnight Commanderből vagy a Finderből): a lapozás a mappája többi
képén halad végig, a Finder sorrendjében. A mappát csak az első lapozáskor olvassa be, így a macOS csak
akkor kér hozzáférési engedélyt a mappához, ha tényleg lapozol.

**Az app továbbadása:** `make zip` vagy `make dmg`. Az app csak ad-hoc aláírást kap, ezért egy másik
Macen elsőre nem engedi megnyitni a rendszer: jobb klikk → Megnyitás (macOS 14-ig), Rendszerbeállítások
→ Adatvédelem és biztonság → Megnyitás mindenképp (macOS 15), vagy `xattr -dr com.apple.quarantine
nitroview.app`. Apple Developer ID-val figyelmeztetés nélkül nyílik: a `make dmg notarize
SIGN_ID="Developer ID Application: Név (TEAMID)" NOTARY_PROFILE=név` aláírja (hardened runtime),
hitelesítteti az Apple-lel a dmg-t, és hozzátűzi a jegyet (előtte egyszer: `xcrun notarytool
store-credentials név`). A [Releases](https://github.com/gereoffy/jpeg-nitro/releases)
dmg-je ilyen hitelesített build: a kiadás tagjéből a `.github/workflows/macos-release.yml` workflow-val
egy GitHub Actions macOS runneren fordítva [mgelei forkjában](https://github.com/mgelei/jpeg-nitro), az
ő Developer ID-jával aláírva (a származását a `gh attestation verify nitroview-vN.dmg --repo
mgelei/jpeg-nitro` ellenőrzi).

**Finder:** a `make app` elkészíti a `nitroview.app`-ot. Másold az Applications mappába, majd a Finderben
egy képen: Információ → Megnyitás ezzel → nitroview → Az összes módosítása. Képeket vagy mappát a
Dock-ikonra vagy az ablakra is rá lehet húzni, vagy a nitroview → Open… (⌘O) menüvel kiválasztani;
a futó néző átveszi az új fájl(oka)t.

**Windows:** a `nitroview.exe` ([src/nitroview_win.c](src/nitroview_win.c), Win32 + Direct3D 11)
ugyanez a néző: ugyanazok a billentyűk, egérfunkciók, nagyítás és ablakméretezés, teljes képernyő,
diavetítés, egy fájlból a mappája, ráhúzás, O / Shift+O: fájlok / mappa megnyitása (fájl nélkül indítva
is ez az ablak jön fel). A bitképes formátumokat a fenti dekóderek vagy a Windows WIC-kodekjei
bontják ki. Az SVG és a CorelDRAW CDR / CMX ugyanúgy, mint macOS-en (nagyítva éles, [ / ] a rajz
oldalai között). Egyetlen önálló .exe (csak rendszer-DLL-ek, Windows 10-től), Macen vagy Linuxon
fordítható a `make windows`-szal (lásd lent: „A dekóderek más rendszeren”). A `nitroview.exe --register`
felajánlja az összes ilyen típushoz az Intézőben (az aktuális felhasználónak, rendszergazdai jog
nélkül), és megnyitja a Beállítások → Alapértelmezett alkalmazások oldalt, ahol ki kell választani
(az alapértelmezést a Windowsban csak a felhasználó választhatja); a `--unregister` visszavonja.
Ha fut, a megnyitott fájlok a futó ablakban jelennek meg (`-n`: új ablakban). Még nincs benne:
színkezelés (a képek sRGB-ként jelennek meg); az érintőpados gesztusok nincsenek kipróbálva.

**GNOME / Linux:** a `nitroview-gnome` ([src/gnome/](src/gnome), GTK 4 + libadwaita, Czirok Ferenc
munkája) ugyanazokat a billentyűket, egér- és érintőpados gesztusokat, nagyítást és mozgatást, teljes
képernyőt, diavetítést (`-s`), egy fájlból a mappája lapozását, ráhúzást és Megnyitás-ablakot tudja.
A bitképes formátumokat a fenti dekóderek bontják ki; a JPEG színkonverziója többszálú,
és pontosan a libjpeg-turbo RGB-kimenetét adja (`bench/rgbverify`), a PNG-sorok másolás nélkül kerülnek
a GTK-hoz, az EXIF-forgatást a rajzolás végzi. A képek háttérszálon dekódolódnak, ugyanazzal az
előtöltéssel, mint macOS-en és Windowson (3 előre, 2 hátra). Minden mást a GTK saját betöltői nyitnak meg.
Az SVG és a CorelDRAW CDR / CMX a nitrosvg-vel / nitrocdr-rel megy (nagyítva éles, [ / ] az oldalak
között, ha a `scripts/get-deps.sh` letöltötte őket). Lapozáskor az ablak mérete nem változik (Wayland alatt egy
program nem méretezheti át szabadon a saját ablakát). A `make gnome`-hoz GTK 4.10+ és libadwaita 1.4+
kell; a `make gnome-register` az aktuális felhasználónak felveszi az asztalra; a `make flatpak` a
GNOME 50 runtime-ra fordítja a `nitroview-gnome.flatpak`-ot.

**SDL3 / Linux (új, tesztelés alatt):** a `nitroview-sdl` ([src/nitroview_sdl.c](src/nitroview_sdl.c))
a windowsos néző felépítése Win32 + Direct3D helyett SDL3 + OpenGL 3.3-mal: ugyanazok a billentyűk és
egérműveletek (+ O: fájlok, Shift+O: mappa megnyitása), ablakkezelés, előtöltés, Del / U, E / Shift+E, SVG / CorelDRAW
éles nagyítással, `--bench` és `--selftest`, és ugyanaz a GPU-út (a síkok textúraként, YCbCr→RGB
shaderben, mipmapek, trilineáris rajzolás; pixelpontos 200–800%). Amit a saját dekóderek nem
vállalnak (interlace-elt PNG, GIF, BMP, sérült vagy CMYK JPEG), azt az
stb_image bontja ki; a HEIC csak `make NITROHEVC=1`-gyel fordítva támogatott (lásd lent). Az SDL3 statikusan van belefordítva, az OpenGL-t és a Waylandet / X11-et futáskor
tölti be, GTK nem kell hozzá. Az SDL3-at a `scripts/get-deps.sh` fordítja (az ehhez szükséges
fejlesztői csomagok ott vannak felsorolva). macOS-en is lefordul, de csak teszteléshez (ott az OpenGL
lassú; ott a Metal-os néző az ajánlott). A `make sdl-dist` készíti a továbbadható egyetlen fájlt, a
`nitroview-linux-x64.tar.gz`-t (4 MB): a fordító gép processzora helyett x86-64-v3-ra (AVX2) fordítva,
a libstdc++-szal, a zlibbel és a dav1d-vel együtt, csak a glibc kell hozzá (Ubuntu 22.04-en fordítva
2.35-ös vagy újabb). 64 bites ARM-os Linuxon (Asahi Linux Apple M1 / M2-n, Raspberry Pi 4 / 5) ugyanez
a `nitroview-linux-arm64.tar.gz`-t készíti, armv8-a-ra fordítva (a NEON-os kódágak ugyanazok, mint a Mac
arm64-es buildjében; HEIC csak `make NITROHEVC=1`-gyel, mint x86-os Linuxon); M1-es MacBook Airen, Fedora Asahi Remix 44 alatt kipróbálva
(KDE Plasma Waylanden, a Mesa M1-es GPU-ra írt OpenGL 4.6-os drivere): a 100% 200%-os és 300%-os kijelzőskálázásnál is pixelpontos. A `nitroview-sdl --register` az aktuális felhasználónak felveszi az asztal alkalmazásai közé
(desktop fájl és ikon a `~/.local/share`-ben: „Megnyitás ezzel” a fájlkezelőben), az `--unregister`
eltávolítja.

**HEVC:** a HEIC-fotók HEIF formátumú HEVC (H.265) képek, a HEVC-re pedig licencet igénylő
szabadalmak vonatkoznak: az operációs rendszerek dekóderei ezzel együtt jönnek, a saját
megvalósításunk nem. Ezért az általunk terjesztett binárisok a HEIC-et csak a rendszer dekóderével
bontják ki: macOS-en a VideoToolbox-szal (a nitroheic párhuzamosan adja át neki a csempéket),
Windowson a WIC-kel (a Microsoft HEVC-bővítményének telepítve kell lennie), a GNOME-os nézőben a GTK
betöltőivel (a GNOME futtatókörnyezet glycin + libheif párosa, a Freedesktop codecs-extra kiterjesztés
HEVC-dekóderével); az SDL-es néző ilyenkor egyáltalán nem mutat HEIC-fájlokat.

A nitro egyszerűsített, csak intra HEVC-dekódere (a deblocking és az SAO szűrő kimarad, fotókon ez
nem látszik), amely a H.265 szabvány alapján készült (`src/nitrohevc.c`), csak a `make NITROHEVC=1`-gyel
fordított buildben érhető el, saját használatra (a szabadalmak kérdése ekkor a fordítóé).

**AVIF** (nitroavif, `src/nitroavif.c`): AV1-es állókép HEIF-tárolóban, egyre több webáruház
és CDN ezt küldi. A HEIF-szerkezetet a saját kódunk olvassa (`src/heif.h`, közös a nitroheic-kel), az
AV1-et a [dav1d](https://code.videolan.org/videolan/dav1d) bontja ki (VideoLAN, BSD-2-Clause; a
`scripts/get-deps.sh` fordítja meson + ninja segítségével, egy saját Python virtuális környezetben), a
színkonverzió a miénk, párhuzamosan: 8 / 10 / 12 bit, 4:2:0 / 4:2:2 / 4:4:4 / szürke, BT.601 / 709 /
2020 / RGB mátrix, teljes vagy szűkített tartomány, alfa fekete háttér fölött, a csemperácsok
párhuzamosan. Mindkét Linuxos néző ezt használja (a GTK-nak külön csomag nélkül nincs AVIF-betöltője),
és a macOS-es is, ha a `scripts/get-deps.sh` lefordította a dav1d-t (univerzálisan: x86_64 + arm64; a
`make DAV1D=0` az AVIF-et az ImageIO-ra hagyja), és a windowsos is, ha a `WINCC=… scripts/get-deps.sh`
lefordította rá (`third_party/dav1d-win`; különben a WIC, az AV1 Store-bővítménnyel). 41, webáruházakból és
hírportálokról mentett AVIF dekódolása (300×200 … 3000×3000), ms/kép: i9 (macOS) ImageIO 31,7 → nitroavif
10,0, M1 22,4 → 10,8, i5-9600K (Windows) 13,0 (a WIC Store-bővítmény nélkül egyiket sem nyitja meg). A pixelek ±1-en belül egyeznek 4:4:4-nél és a
világosságban, 4:2:0-nál a színes éleken eltérnek, mert az ImageIO ismétli a chroma-mintákat, mi
lineárisan interpolálunk (mint a JPEG-nél). Ellenőrizve az AOM tesztfájljain
([av1-avif](https://github.com/AOMediaCodec/av1-avif)) és a [Link-U mintáin](https://github.com/link-u/avif-sample-images)
a PNG-referenciáikkal: minden bitmélység, chroma-formátum, fekete-fehér, szűkített tartományú alfa,
rács, többrétegű kép (a1op, több darabban tárolt adat), vágás (forgatás után is, a tulajdonságok
sorrendjében) és tükrözés egyezik; az ImageIO ezek közül többet rosszul jelenít meg (szűkített
tartományú szürke és alfa, forgatás utáni vágás, a 2-es profilú fájlokat ki sem bontja). Nem kezeli:
rétegzett csempékből álló rács (két Apple-tesztfájl). Linuxon és bármennyi szálon ugyanaz a kimenet;
sérült fájlokon AddressSanitizer alatt is hibátlan. (Közben javult a nitroheic `imir`-tengelye is: a 0
a felső és alsó részt cseréli.)

Az ablak a képhez igazodik: ha a kép kisebb a képernyőnél, pontosan 100%-on, egyébként a kép
arányával, akkorára, amekkora kifér (fekete sávok nélkül). Nagyításkor az ablak a képpel együtt nő, a
képernyő méretéig (kicsinyítéskor visszamegy, a 0 visszaállítja). Lapozáskor minden képnél
igazodik (a közepe helyben marad), kivéve ha bele van nagyítva: akkor az ablak, a nagyítás és a
pozíció is marad. Ha kézzel átméretezed az ablakot (szél húzása, zöld gomb, ablakrendezés), azt a
méretet megtartja; a W visszaállítja a kép méretére, és újra bekapcsolja az automatikus méretezést. A címsorban a fájlnév után látszik a nagyítás mértéke. Egész
számú nagyításnál (200%, 300%, 400%…, görgővel vagy +/−-szal elérve is) a képpontok pontos
négyzetekként látszanak, a többi nagyítás simított.

**Lapozáskor a nagyítás és a pozíció megmarad**, így egy sorozatképnél bele lehet nagyítani
egy részletbe (pl. a szembe), és végiglapozva kiválasztható, melyik a legélesebb. A görgős nagyítás
helyben tartja a kurzor alatti pontot, kivéve amíg a kép valamelyik irányban keskenyebb az
ablaknál: abban az irányban addig középen marad, amíg ki nem tölti. Ha a Ctrl + görgő a teljes képernyőt nagyítja, akkor a macOS
kisegítő nagyítása van a Ctrl + görgetésre állítva (Rendszerbeállítások → Kisegítő lehetőségek →
Nagyítás). Ha a csíptetés semmit nem csinál, a gesztus ki van kapcsolva (Rendszerbeállítások →
Trackpad → Görgetés és nagyítás → Nagyítás és kicsinyítés). A nagyító
billentyűket a leütött karakter alapján ismeri fel, így bármilyen billentyűzetkiosztáson és a
numerikus billentyűzeten is működnek; a címsor mutatja az aktuális nagyítást.

Egyéb módok:
- `--bench fájlok…`: ablak nélkül betölti az összeset (olvasás, dekódolás, GPU), és időt mér.
- `--selftest fájlok…`: a GPU-s színkonverziót összeveti a libjpeg-turbo kimenetével.
- `--zoomtest fájlok…`: a nagyítás/mozgatás billentyűsorozatait képernyőn kívül lejátssza, és
  ellenőrzi, hogy 100%-nál minden kirajzolt pixel egyezik a dekódolt képpel (a szélek és a
  nagyítási lépések igazítása is).
- `--inputtest fájlok…`: mesterséges egér- és görgőeseményeket küld, és ellenőrzi a lapozást és a
  nagyítást.
- `--auto <ms>`: tesztelési mód: adott időközönként lapoz (akkor is, ha a kép még nem jelent
  meg), a végén kilép. Késleltetésméréshez.

## Eredmény (50 minta, zömmel 6000×4000, átlag 17 MB)

**Teljes betöltés a nézőben: ~23 ms/kép** (fájlolvasás 3.5 + dekódolás 12.7 + GPU 7).
Lapozáskor az előtöltött kép **7–20 ms** alatt kerül a képernyőre (1 képkocka 60 Hz-en).

### Dekóderek összehasonlítása (`bench/bench`, csak dekódolás, memóriából)

| dekóder | ms/kép |
|---|---|
| Apple ImageIO (CGImageSource) → BGRA | 274 |
| Apple ImageIO, saját puffer | 249 |
| stb_image | 175 |
| Wuffs | 159 |
| ImageIO thumbnail 3000 px (DCT-skálázás) | 159 |
| VideoToolbox (JPEG; ezen a gépen nincs HW JPEG, szoftveres) | 157 |
| libjpeg-turbo 3.1.2 (AVX2), BGRX | 124 |
| libjpeg-turbo, fast DCT + fast upsampling | 112 |
| libjpeg-turbo → YUV síkok (nincs színkonverzió) | 107 |
| libjpeg-turbo 1/2 skála | 100 |
| libjpeg-turbo 1/4 skála | 96 |
| nitrojpeg, saját motor, **1 szál** → YUV | 103 |
| nitrojpeg, régi motor (libjpeg-turbo dekódolja a sávokat) → YUV | 20.6 |
| **nitrojpeg, saját motor, párhuzamos → YUV síkok** (ezt használja a néző) | **11.7** |

A 1/4-es skálázás alig gyorsít, tehát az idő nagy része a Huffman-dekódolás,
ami a JPEG-ben eredendően soros. A mintaképek többsége nem tartalmaz restart
markert, ezért a libjpeg-turbo egy képen belül nem tud párhuzamosítani.
Az RX 580-hoz nincs hardveres JPEG-dekóder.

### Apple Silicon (M1)

Egy 2020-as M1-es MacBook Air (4 nagy teljesítményű + 4 energiatakarékos mag, ventilátor nélkül),
`bench/bench` a fotók közül 43-on (`samples/*.JPG`, 765 MB), ms/kép. A nyers kimenet, az i9-cel is
ugyanerre a 43 fájlra: `bench/results/m1.txt`.

| dekóder | ms/kép |
|---|---|
| stb_image | 384 |
| Wuffs | 217 |
| libjpeg-turbo, BGRX | 185 |
| libjpeg-turbo → YUV síkok | 168 |
| Apple ImageIO → BGRA | 108 |
| Apple ImageIO, saját puffer | 89 |
| VideoToolbox | 88 |
| ImageIO thumbnail 3000 px | 67 |
| nitrojpeg, **1 szál** → YUV | 154 |
| **nitrojpeg, párhuzamos → YUV** | **30.4** |

Az M1-en az ImageIO és a VideoToolbox hardveresen dekódolja a JPEG-et: 2.5–3×-osan gyorsabban,
mint az i9-en a szoftveres út (fenti táblázat). A nitrojpeg (NEON IDCT, 8 szál) teljes méretben
ennél a hardvernél is ~3×-osan, a libjpeg-turbónál 5.5×-ösen gyorsabb. Ebben a munkában egy M1-es
mag ~1.5×-ösen lassabb egy i9-es magnál (1 szálon 154 ms, az i9-en 103 ms a fenti táblázatban); a szálak jól skálázódnak: 2 szálon 81 ms,
4-en 46 ms, 6-tól 33–36 ms (a négy energiatakarékos mag együtt nagyjából egy nagy magnyit ad).
A NEON-os IDCT 8 szálon 43-ról 31 ms-ra gyorsított. Ott az alapbeállítás (magonként egy szál, 8)
a legjobb.

PNG (`bench/pngbench samples_png/real/*.png`: 34 valódi PNG, 938 MB, 805 megapixel —
képernyőképek, szkennelések, AI-képek, térképek; ms/kép):

| dekóder | M1 | i9 |
|---|---|---|
| Apple ImageIO, saját puffer | 364 | 345 |
| Apple ImageIO → BGRA | 392 | 376 |
| Wuffs → BGRA | 272 | 180 |
| nitropng, 1 szál | 326 | 223 |
| **nitropng, párhuzamos** | **90** | **45.5** |

PNG-hez nincs hardveres dekóder, így az ImageIO mindkét gépen ugyanolyan lassú. Az M1-en a
nitropng 3×-osan gyorsabb a Wuffs-nál és 4×-esen az ImageIO-nál. Az Adler-32 és a
visszahivatkozások feloldása ott NEON-t használ (x86-on AVX2-t): ez ~100-ról 90 ms-ra gyorsított;
az idő nagy része a Huffman-dekódolás és a sorszűrők visszafejtése, ezek mindkét gépen sima C-ben futnak.

HEIC (egy kis tesztprogrammal, ami nincs a repóban: 20 iPhone-fotó 48 csempéje, 12 MP, a VideoToolbox-szal különféle
módokon, a dekóderek létrehozásával együtt; az utolsó sor: `nitroview --bench` mind a 155-ön; ms/kép):

| dekóder | M1 | i9 |
|---|---|---|
| Apple ImageIO | 58–60 | 176 |
| VideoToolbox hardveres, 1 szál | 52 | 135 |
| VideoToolbox hardveres, 4 (M1) / 8 (i9) szál | **24–29** | 224 |
| VideoToolbox szoftveres, 4 / 8 szál | 62–75 | **42** |
| **nitroheic** (a viewerben) | **25** | **33** |

Az M1 médiamotorja egyszerre 4 csempét szolgál ki, az i9 melletti Radeon RX 580 egyszerre egyet
(több szálon lassabb), a szoftveres dekóder pedig a magokkal skálázódik: ezért a nitroheic Apple
Siliconon a hardvert, Intelen magonként egy szoftveres dekódert használ.


Progresszív JPEG (`nitroview --bench` 100 darab, 1 MB-nál nagyobb progresszív webes JPEG-en, átlag
5.8 MP; dekódolás / a GPU-ra töltéssel és konverzióval együtt, ms/kép):

| dekóder | M1 | i9 | i5-9600K, Windows |
|---|---|---|---|
| libjpeg-turbo (TurboJPEG, a néző korábbi útja macOS-en) | 43.4 / 47.1 | 41.9 / 47.9 | |
| WIC (a néző korábbi útja Windowson) | | | 118 / 128 |
| **nitrojpeg** (a menetek csővezetékben) | **10.6 / 15.8** | **9.5 / 14.3** | **11.0 / 18.3** |

Az M1-en 4×, az i9-en 4.4×, az i5-ön 10.7× gyorsabb (a WIC-nél; 6 mag hyper-threading nélkül, a
windowsos néző mipmapeket is készít, 1.5 ms). Az i5-ön egy szálon 29.3 ms (`bench/nbench -1`),
4 workerrel 11.0, 6-tal (`-j 6`) 11.8. A 16 bites IDCT (pmaddwd / vmlal) csak egy szálon számított
(az i9-en lassúak a 32 bites vektoros szorzások); a 4 worker az összes mag helyett az M1-et
11.3-ról 10.6 ms-ra, az i9-et 11.7-ről 9.5 ms-ra gyorsította (többet nem lehet munkával ellátni,
lásd: Progresszív JPEG; a `-j` többet is beállíthat).
### Három gép, macOS és Linux

`bench/nbench` (hordozható, minden rendszeren ugyanaz a kód), 5 dekódolásból a legjobb, ms/kép;
JPEG: az 50 mintafotó, PNG: a fenti 34 valódi PNG. Az M1 sora a `bench/bench` / `bench/pngbench`
méréséből van (JPEG: 43 fotó).

| gép | JPEG, párhuzamos | JPEG, 1 szál | PNG, párhuzamos | PNG, 1 szál |
|---|---|---|---|---|
| Intel i9, 13. gen., 8 mag / 16 szál, macOS | **8.7** | 96 | **37.6** | 216 |
| Apple M1, 4 + 4 mag, macOS | **30.4** | 154 | **90** | 326 |
| Intel Xeon E3-1245 v5 (2015), 4 mag / 8 szál, Linux | **33.6** | 161 | **122** | 390 |

Linuxon a dekóderek a `src/nitro_os.h` pthreads-es szálkészletén futnak, és ott is bitpontosak
(`bench/verify`, `bench/pngverify`). Egy tízéves, 4 magos szerver egy 24 MP-es fotót ~34 ms alatt
dekódol, nagyjából mint az M1; a hyper-threading JPEG-nél ~20%-ot ad (4 magon 4.8×), PNG-nél
keveset (3.1×).

Intel Core i7-1260P (4 P + 8 E mag / 16 szál, Arch Linux, 64 GB RAM), `bench/nbench`,
5 dekódolásból a legjobb, másik képkészleten (4 JPEG, 5 PNG): JPEG **5.4 ms** párhuzamosan /
32.3 ms 1 szálon; PNG **13.4 ms** / 35.4 ms. Egy 12000×12000-es (144 MP) RLE PSD:
**68.2 ms** / 112.4 ms 1 szálon. A képkészlet eltér a fenti tábláétól, ezért ezek az értékek
nem hasonlíthatók közvetlenül annak három sorához.

Ugyanezen a gépen, 5 futás legjobb értékével, egy 6000×6000-es JPEG és egy 2000×1970-es PNG:

| dekóder | JPEG | PNG |
|---|---:|---:|
| **Nitro** | **14.8 ms** | **12.9 ms** |
| Glycin 2.1.0 betöltés + frame | 184.5 ms | 37.6 ms |

Ez JPEG-nél **12.5×**, PNG-nél **2.9×**. A Nitro érték a `bench/nbench` dekódolási ideje;
a Glycin érték a `gly_loader_load()` + `gly_image_next_frame()` idejét tartalmazza.

### TIFF (nitrotiff)

A `src/nitrotiff.c` a TIFF 6.0 szabvány alapján készült: tömörítetlen, PackBits, LZW és Deflate (a
vízszintes predictorral), a fekete-fehér képekhez CCITT G3 (MH, T.4 1D / 2D) és G4; 1–16 bites szürke,
palettás, RGB és CMYK (egyszerű képlettel, profil nélkül), alfa fekete háttér fölött, csíkok vagy
csempék, chunky vagy planar elrendezés. A csíkokat és csempéket párhuzamosan bontja ki, utána a
sorokat párhuzamosan alakítja BGRX-re; a tömörítetlen képet (a szkennerek ilyet írnak: 16 bites RGB,
70 MB egyetlen csíkban) közvetlenül a fájlból alakítja át. A JPEG-tömörítésű és YCbCr TIFF-ek a
rendszer dekóderéhez kerülnek. macOS-en az RGB TIFF ICC-profilját használja, ahogy az ImageIO is.

A `bench/bgrxverify` (macOS) az ImageIO-val veti össze: 63 szken, fax, ikon és dokumentum, valamint
33 ImageMagick-kel és libtiff-fel készült változat (minden tömörítés, csempék, planar, 16 bit, alfa,
bitsorrend) pixelre azonos (palettás képeknél ±1: a 16 bites színtábla kerekítése; a CMYK eltér, mert
az ImageIO profillal alakít). Linuxon és bármennyi szálon is ugyanaz a kimenet; sérült fájlokon
(csonkított, összekuszált könyvtárak) AddressSanitizer alatt is hibátlanul fut. A nézőben (i9, a 63
valódi TIFF) a dekódolás 14,0-ről 3,1 ms/képre gyorsult: egy 13 MP-es 16 bites szken ~58-ról ~2,3
ms-ra, egy 17 MP-es LZW-s rajz ~46-ról ~6 ms-ra; az egyetlen csíkból álló G4-es faxok sorosak
maradnak (~2 ms).

### nitrojpeg: hogyan párhuzamosít egyetlen képen belül?

A saját motor (alapértelmezés) a Huffman-folyamot **egyszer** dekódolja, libjpeg-turbo nélkül:

1. **Előkészítés (párhuzamos, ~1 ms):** megkeresi az adat végét, eltávolítja a
   byte-stuffinget (0xFF 0x00) és a restart markereket, így egy „tiszta” bitfolyam lesz.
2. **Marker nélküli fájlok: spekulatív teljes dekódolás.** A bitfolyamot ~512 darabra
   vágja (`32 × CPU`, darabonként ~200 KB együttható, ez befér az L2 cache-be). Minden
   darabot egy szál egy tetszőleges bájtpozíción kezd dekódolni, és az együtthatókat
   rögtön kiírja. A Huffman-kódok néhány MCU után „önszinkronizálnak” a valódi úttal.
3. **Összefűzés, sorrendben, futószalagon.** Ha a valódi út egy olyan bitpozíción kezd
   MCU-t, amit a következő darab is feljegyzett, onnantól azonosak. A darab MCU-sorszáma
   és komponensenkénti DC-eltolása így kiderül, a szinkron előtti pár MCU eldobva.
4. **Dekvantálás + IDCT** saját AVX2 kóddal. Az algoritmus a libjpeg „ISLOW” IDCT-je
   (jidctint.c), ezért bitre azonos kimenetet ad. Egy szál, amint egy általa dekódolt
   darab összefűződött, azonnal IDCT-zi, amíg az együtthatók még a cache-ben vannak.
5. **Restart markeres fájlok:** nincs spekuláció, az intervallumokat párhuzamosan
   dekódolja, és MCU-nként rögtön IDCT-zi.
6. A kimenet Y/Cb/Cr síkok, közvetlenül egy Metal (shared) pufferbe. A felmintázást
   és a YCbCr→RGB konverziót egy GPU compute kernel végzi, a mipmapeket
   a GPU generálja, a megjelenítés trilineáris szűréssel skáláz.

Mért szinkronizáció (`bench/verify`, 512 darab, képenként az első 256 szinkronpont,
40 kép = 10240 eset): átlag 2.40 „szemét” MCU (~922 bit, ~115 bájt) után minden darab
rááll a helyes útra. Az esetek 40%-ában 1 MCU után, a leglassabb 21 MCU (8754 bit) volt,
és egyetlen esetben sem maradt ki a szinkron.

**Pontosság:** a párhuzamos kimenet **bitre azonos** az egyszálú libjpeg-turbo
kimenetével, mind az 50 képen (`bench/verify`, skalár és AVX2 IDCT-vel is). A GPU-s
konverzió legfeljebb ±1 eltérést ad (kerekítés, `--selftest`).
Nem támogatott formátumoknál (CMYK, RGB, aritmetikai kódolás, >16384 px), sérült és félbevágott
fájloknál a néző a rendszer dekóderére vált (ImageIO, WIC, GTK). Az évek során letöltött ~4100
JPEG közül egyiknél sem.

### A fejlődés lépései (ms/kép, 16 szál)

| változat | ms/kép |
|---|---|
| libjpeg-turbo, 1 szál | 107 |
| spekulatív átugrás + libjpeg-turbo dekódolja a 128 „kamu” JPEG sávot (Huffman kétszer) | 19.9 |
| ugyanez, de egyszálú Huffman-bejárás a spekuláció helyett (`NJ_SEQ=1`, régi motor) | 67.2 |
| saját motor: egyszeri Huffman, együtthatók a memóriában, utána IDCT | 15.7 |
| + egy-lookupos AC-dekódolás, futószalag: IDCT amíg cache-ben van | **11.7** |

Az egyszálú Huffman-bejárás azért lassú, mert soros függőségi lánc: minden kód hosszát
ismerni kell, mielőtt a következőt olvasni lehetne (~12 órajel/szimbólum). A futószalag
nélkül az IDCT memória-korlátos volt: a 96 MB-os együtthatótömb kiírása és visszaolvasása
~45 GB/s-nál telítődött, 8 szál fölött már nem gyorsult.

Darabszám a saját motorban (`NJ_CHUNKS`), ms/kép:

| darabok | 64 | 128 | 256 | 512 | 1024 | 2048 |
|---|---|---|---|---|---|---|
| ms/kép | 12.3 | 12.0 | 11.9 | **11.4** | 12.1 | 14.7 |

A régi motor (libjpeg-turbo sávok, `nj_set_engine(1)`, `NJ_ENGINE=1`) mérései a
`bench/results/matrix.txt` (darabok × szálak) és `bench/results/bands.txt` (sávszám) fájlokban vannak.
Ott a HT 8 → 16 szálon ~26%-ot hozott, és a sok kis sáv volt a jobb.

### Mérési módszer: órajel és hőmérséklet

A gép (hackintosh, i9-13. gen.) órajele 4 és 5.5 GHz között ugrálhat, 16 szálas terhelésnél
a CPU gyorsan 90 °C fölé melegszik, ezért egy-egy mérés ±10%-ot is tévedhet.

- `bench/freqprobe [szálak] [másodperc]`: root nélkül méri a valódi órajelet (egymástól
  függő összeadások lánca: 1 összeadás/órajel). Mért értékek: egy szálon, pihenő gépről
  4.48 GHz (az ütemező lassan emeli az órajelet), 16 szálon ~4.95 GHz, 8 szálon ~5.3 GHz.
- `bench/sustain SEC SZÁLAK fájlok…`: folyamatos dekódolás, félmásodpercenként sebesség +
  órajel. 30 másodpercig 16 szálon nincs lefelé tartó trend (10.1–11.9 ms/kép, ±10%
  ingadozás), tehát a hűtés tartja; a szórást az órajel-ugrálás okozza, nem fojtás.
- `bench/ab.sh ISMÉTLÉS SZÜNET -- "név:ENV=érték" …`: a változatokat felváltva futtatja,
  minden futás előtt hűlési szünettel és órajelméréssel, mediánt/min/max-ot ad.
  Így a min–max tartomány jellemzően ±3%-on belül marad.

Újramérve ezzel a módszerrel (5 ismétlés, 3 s szünet, ms/kép, `bench/results/ab_results.txt`):

| összehasonlítás | medián (min–max) |
|---|---|
| saját motor / régi motor (libjpeg-turbo sávok) | **11.8** (11.8–11.9) / 19.8 (19.5–20.8) |
| 4 / 8 / 12 / 16 szál | 27.0 / 15.4 / 12.3 / **11.8** |
| 256 / 512 / 1024 darab | 11.9 / **11.7** / 12.2 (átfedő tartományok: a különbség zajon belül) |

A következtetések kitartanak: a saját motor 1.7× gyorsabb a réginél, a HT 8 → 16 szálon
~30%-ot hoz, a darabszám 256–1024 között lényegtelen. (Minden futás külön folyamat, hideg
pufferkészlettel, ezért ~1 ms-mal lassabb a nézőben mért meleg állapotnál.)

### Progresszív JPEG

A progresszív JPEG sok menetből (scan) áll (általában 9–10): a DC-együtthatók, az AC-együtthatók
sávjai, majd finomító menetek, amelyek minden együtthatóhoz még egy bitet adnak. Minden menet külön
Huffman-folyam, a szokásos dekóderek egymás után dekódolják őket. Menetenként mérve (libjpeg-turbo)
1521 progresszív webes képen és 50, `jpegtran -progressive`-vel átalakított 24 MP-es fotón a Y AC
finomítás az idő ~60%-a, a kimenet (IDCT, szín) 6–15%. A független menetek párhuzamos dekódolása
csak 1.3–1.6×-ot hozna, mert a Y-menetek egymásra épülnek. Egy menetnek viszont csak az ugyanazon
együtthatókat érintő korábbi menetek *ugyanazon blokksoraira* van szüksége, ezért a nitrojpeg a
meneteket csővezetékben (hullámfrontban) futtatja: minden menet néhány MCU-sorral halad előre,
amint az előtte lévők elég messze járnak, és az IDCT követi az utolsókat. Így az idő felső határa a
leghosszabb menet, nem az összes menet összege. A finomító meneteket is máshogy dekódolja: a
libjpeg együtthatónként lépked végig a sávon (minden nemnullánál egy javítóbit, a nullákat
számolja), a nitrojpeg egyszer kigyűjti a blokk nulla együtthatóit, és a futás végére ugrik; ettől
egy szálon is 1.5× gyorsabb. Alapból 4 workerrel dolgozik (a leghosszabb menet a munka negyede-
harmada, több szálnak nem jut munka, csak lassítanak; a `-j` többet is beállíthat):

| ms / MP | nitrojpeg | nitrojpeg, 1 szál | libjpeg-turbo (Y/Cb/Cr) | libjpeg-turbo (BGRX) |
|---|---|---|---|---|
| 50 × 24 MP-es fotó (jpegtran) | **3.31** (~79 ms) | 9.35 | 14.02 (~336 ms) | 14.32 |
| 1509 webes kép (átlag 2.7 MP) | **1.66** | 3.50 | 5.33 | 5.61 |

A kimenet mindegyiken bitre azonos a libjpeg-turbóéval (a `bench/verify` a progresszív fájlokat
annak Y/Cb/Cr síkjaival veti össze, minden szállal és eggyel is). További 12 webes kép (nincs a
táblázatban) alacsony minőségű előnézet, hiányoznak az utolsó menetei, némelyikben csak a DC van
(blokkonként egy szín): a nitrojpeg azt dekódolja, ami benne van, ahogy az Apple ImageIO is. A
libjpeg-turbo a hiányzó alacsony AC-együtthatókat a szomszédos blokkokból becsüli is („block
smoothing”, lágyabb, de elmosódott); a nitrojpeg nem, a `bench/verify` ezeket a simítás nélküli
libjpeg-turbóval veti össze (azonos, 270, az első meneteire visszavágott tesztfájlon is). A sérült
és félbevágott fájlokat a rendszer dekódere kapja.

### nitropng

A PNG deflate (LZ77 + Huffman) és soronkénti szűrők, mindkettő eredendően soros:

1. **Párhuzamos, spekulatív kicsomagolás (mint a *pugz*):** a tömörített adat szálanként egy
   darabra oszlik. Minden szál megkeresi a darabjában az első érvényes deflate-blokkfejlécet (a
   Huffman-tábláknak pontosan teljesnek kell lenniük, így a hamis találat ritka, és úgyis kiesik),
   és onnan dekódol. A még ismeretlen előző 32 KB-ba mutató visszahivatkozások helyőrzők lesznek
   (16 bites szimbólumok). A darabokat sorrendben fűzi össze; csak mindegyik utolsó 32 KB-ját kell
   sorban kitölteni, a többit párhuzamosan (AVX2). A teljes kimenet Adler-32-jének (a darabok
   összegeiből, AVX2) egyeznie kell, különben a néző tartalékdekóderre vált.
2. **Párhuzamos szűrő-visszaállítás („hullámfront”):** a None/Sub sorok függetlenek; az Up/
   Average/Paeth sorok szakaszonként (1 KB) követik az előző sort, 16-ból 12 szálon (a várakozó
   hyperthread-ek lassítanák a dolgozó testvérszálukat). Teljesítmény- és hatékonysági magokkal
   (Apple M1, Intel 12. generáció) csak a teljesítménymagokon: egy lassabb magon futó sor minden
   utána következőt feltart (az M1-en Linux alatt 6 helyett 4 szál, kb. +20%). Linux alatt a
   dekóderek minden olyan párhuzamos ciklusa, amelynek legfeljebb annyi eleme van, ahány
   teljesítménymag, azokon fut (`src/nitro_os.h`; a WebP három feladata is: +15% az M1-en).
3. A visszaszűrt sorokat a GPU közvetlenül olvassa (a szűrőbájtot átugorva), előszorozza az
   alfát és RGBA-ra alakít — a CPU-n nincs színkonverzió.

`bench/pngverify`: bájtra pontos összevetés a zlib + egyszerű szűrő-visszaállítással (mind a 33
támogatott teszt-PNG azonos), `bench/pngrobust`: sérült PNG-k ASan + UBSan alatt (nincs hiba),
`--selftest`: a teljes út az ImageIO-val összevetve (pixelpontos).

### PNG: miért nem volt párhuzamos

`bench/pngbench` (dekóderek) és `bench/pngsplit` (hová megy az idő), PNG-be mentett 24 MP-es fotókon:

| dekóder | ms/kép |
|---|---|
| Apple ImageIO | 339 |
| Wuffs | 193 |
| csak a zlib-es kicsomagolás | 91 |
| csak a szűrők visszaállítása (sima C) | 67–134 |

A PNG deflate (LZ77 + blokkonként változó Huffman-táblák) és soronkénti szűrők. A folyam közepén
induló szál nem ismeri az előző 32 KB kimenetet, amire a visszahivatkozások mutatnak, és a sorok
97%-a Paeth-szűrős, ami az előző sortól függ. Elvileg mindkettő párhuzamosítható: spekulatív
kicsomagolás utólag kitöltött visszahivatkozásokkal (mint a *pugz*), és „hullámfront”
szűrő-visszaállítás, ahol az r. sor az (r−1). mögött halad. Ez egy 24 MP-es PNG-t talán
20–40 ms-ra vihetné le (az akkori becslés; hogy mi lett belőle, lásd fent: nitropng).

### Hibás fájlok

Ha a dekóder hibát lát (érvénytelen Huffman-kód, túlfutó együtthatóindex, az utolsó
MCU a bitfolyam vége után ér véget, eltér a restart markerek száma stb.), nem próbálkozik
tovább. Ilyenkor a néző a rendszer dekóderére vált (Apple ImageIO, Windowson WIC, GTK
betöltői), ami a sérült képből is megmutatja, ami menthető (pl. egy félig letöltött fájl felső
részét); `TURBOJPEG=1`-gyel fordítva a macOS-es néző előbb a TurboJPEG-et próbálja (ezeken kb.
2× gyorsabb). Ha egyik sem tudja dekódolni (ismeretlen formátum, sérült fejléc, üres fájl), a
címsorban `CANNOT DECODE` jelenik meg, és a lapozás folytatható.

`make bench/robust && bench/robust samples/*`: minden mintából ~90 rontott változatot
készít (csonkítás 0 bájttól a teljes hossz −1-ig, véletlen bájt- és bithibák, nullázott vagy
0xFF-fel kitöltött blokkok, adatba szúrt markerek, fejléchibák, kivágott/duplázott szakaszok),
plusz szemét- és szövegfájlokat. Mindegyiket a néző dekódolási útján viszi végig,
AddressSanitizerrel és UBSan-nal fordítva: 4372 eset, 0 hiba. A futószalag szinkronizációját
ThreadSanitizer is ellenőrizte (ép és sérült fájlokon).

### Megjelenítés, előtöltés

- A háttérszál a lapozás irányában 3, visszafelé 2 képet dekódol előre.
  A szomszéd képeket előre fel is tölti a GPU-ra és konvertálja.
- A Metal puffereket és textúrákat újrahasznosítja. A 128 MB-os textúra minden képnél
  újbóli lefoglalása ~6 ms volt.
- Shared (rendszermemóriás) puffer + a síkokat közvetlenül olvasó kernel volt a
  leggyorsabb (8 ms, szemben a managed pufferes / textúrás 10 ms-mal).
- A dekóder nagy pufferei (tiszta bitfolyam, együtthatók) képről képre újrahasznosulnak.
  Hideg készlettel (az első 50 kép) a dekódolás 11.9 ms/kép, bemelegedve 10.6–10.8 ms/kép.

### A dekóderek más rendszeren (Linux, MinGW)

**Mind egy könyvtárban.** A `make lib` a `build/libnitro.a`-t készíti el: a fenti dekóderek, a nitroheic, a nitrowebp,
a nitrotiff (dav1d-vel a nitroavif) és a `src/nitro_load.c/.h`, a nézők közös útja a fájltól a dekódolt
képig: az `nv_read_image()` azt olvassa be, ami kell (egy PSD összevont képét, egy nyers kamerafájl beágyazott
JPEG-jét), az `nv_decode()` felismeri a formátumot és a megfelelő dekóderrel egy pufferbe dekódol, az
`NvImage` leírja, mi van benne (YUV-síkok, egy PSD síkjai, B G R X pixelek vagy egy PNG sorai, az EXIF
orientáció, az ICC-profil). A `src/nitro_convert.c/.h` (`nc_yuv_to_rgb()`, `nc_psd_to_rgb()`) a síkokból
RGB-t csinál, párhuzamosan, annak, akinek nincs rá GPU-shadere. Az `examples/nitroconvert.c`
(`make examples/nitroconvert`) az egész száz sorban: bármilyen, a könyvtár által dekódolt képből 24 bites BMP,
az orientációt alkalmazva.

A dekóderek nem függnek a macOS-től: amit a rendszerből használnak, az a `src/nitro_os.h`-ban
van — párhuzamos ciklus (egy kis pthreads-es szálkészlet, minden rendszeren ugyanaz), óra és a
processzormagok száma. A ciklusmagok clang-blokkok (`^(size_t i) {…}`),
ezért **clang kell `-fblocks`-szal** (a GCC nem ismeri a blokkokat); blokk-futásidejű könyvtár
nem kell hozzá. Linuxon a `make` (= `make linux`) a `build/lib/libnitro.so`,
`build/lib/libnitro.a` könyvtárakat és a hordozható eszközöket fordítja le: `bench/nbench`,
`bench/pngverify`, `bench/psdverify`, az ASan-os robusztussági tesztek, valamint a
`scripts/get-deps.sh` után a `bench/verify` és a `bench/robust` (a szkript a libjpeg-turbo 3-at a
`third_party/`-be fordítja; a TurboJPEG 3 API kell, a régebbi rendszercsomagokban nincs meg). Kézzel
például:

```
clang -O3 -march=native -fblocks -c src/nitrojpeg.c src/nitropng.c src/nitropsd.c
clang -O3 -fblocks bench/pngverify.c src/nitropng.c -lz -lpthread -o pngverify   # bájtpontos ellenőrzés a zlib-bel
clang -O3 -march=native -fblocks bench/nbench.c src/nitrojpeg.c src/nitropng.c src/nitropsd.c -lpthread -o nbench
./nbench -1 fotok/*.jpg kepek/*.png   # sebesség: fájlonként legjobb / átlagos idő, -1: egy szálon is
```

A `bench/nbench` (macOS-en `make bench/nbench` is) a hordozható benchmark: minden JPEG-, PNG- vagy
PSD-fájlt beolvas a memóriába, és többször dekódolja (`-r`, alapból 5); a `-j N` a szálak számát
korlátozza.

A `make gnome` a `build/lib/libnitro.a` dekódereit belefordítva készíti el a `nitroview-gnome`-ot
(GTK4 + libadwaita; egyetlen bináris, futáskor nem kell könyvtárat keresnie). A JPEG-, PNG- és PSD/PSB-fájlokat a Nitro dekóderekkel nyitja meg;
fájl nyitható parancssorból, az asztali környezetből vagy a natív Megnyitás párbeszédablakkal.
A `make gnome-register` az aktuális felhasználónak telepíti a desktop fájlt és az ikont.

macOS-en a dekóderek 2026 októberéig a Grand Central Dispatch-et használták; a pthreads-es
szálkészlet azért váltotta fel, hogy minden rendszeren ugyanaz a kód fusson. Ugyanolyan gyors: a
néző `--bench`-e 10 formátumon az M1-en a PNG-nél 7%-kal gyorsabb, a HEIC-nél (VideoToolbox-szal)
5%-kal lassabb, a többinél 2%-on belül; az Intel i9-en nincs különbség. Minden ellenőrzésen
átmegy (`verify`, `pngverify`, `psdverify`, az ASan-os robusztussági tesztek; ThreadSanitizer négy,
egyszerre minden formátumot dekódoló szállal: tiszta, kivéve a VideoToolbox saját kimeneti
puffereinek olvasását, amelyek írását nem látja; ugyanígy a GCD-vel). Linuxon kipróbálva (lásd:
„Három gép, macOS és Linux”).

**Windows:** a `make windows WINCC=…/llvm-mingw/bin/x86_64-w64-mingw32-clang` az
[llvm-mingw](https://github.com/mstorsjo/llvm-mingw)-vel (clang + MinGW-w64; az MSYS2 CLANG64
környezetében is megy) lefordítja a `nitroview.exe`-t (a nézőt, ikonnal), a `bench/nbench.exe`-t és a
`bench/psdverify.exe`-t: statikusan,
csak rendszer-DLL-ekkel, AVX2-vel (`WINARCH=x86-64-v2`: anélkül, AMD FX-re, a Haswell előtti Intelekre;
az AVX2-es build ilyen processzoron ezt kiírja, nem csak csendben kilép; váltáskor a `build/win/*.a`
törlendő, az SVG / CorelDRAW könyvtárak is erre fordulnak). A nézőben az AVIF-hez előbb a
dav1d-t kell Windowsra lefordítani: `WINCC=… scripts/get-deps.sh` (ugyanazzal a fordítóval). Az `nbench.exe` Windows Serveren
is fut; a `*.jpg`-t maga a program bontja ki, és könyvtárnevet is elfogad.

## Build

Linuxon a `make linux` a megosztott/statikus dekóderkönyvtárat és a hordozható eszközöket, a
`make gnome` a GTK4/libadwaita nézőt fordítja; a `make gnome-register` az aktuális felhasználónak
regisztrálja.

macOS-en előfeltétel: Xcode Command Line Tools.

```
make                 # néző; amit a nitrojpeg nem tud (sérült, CMYK ...), az ImageIO-hoz megy
make TURBOJPEG=1     # ezekhez a libjpeg-turbóval (third_party/-ből), és --selftest
make WUFFS=1         # Wuffs az interlaced PNG-khez az ImageIO előtt (minden mást a nitropng dekódol)
make DAV1D=0         # AVIF az ImageIO-val a nitroavif + dav1d helyett (a scripts/get-deps.sh fordítja)
make SVG=0           # SVG (lunasvg) nélkül
make CDR=0           # CorelDRAW (libcdr) nélkül
make app             # nitroview.app a Finderhez: universal (Intel + Apple Silicon), macOS 12-től,
                     # ad-hoc aláírással, ikon: packaging/icon.png
make zip / make dmg  # nitroview.zip / nitroview.dmg (Alkalmazások mappába húzható) továbbadáshoz
make tools           # bench/bench, bench/verify, bench/robust (referenciának kell a libjpeg-turbo)
```

**A nézőnek nem kell a libjpeg-turbo.** A dekóder (`src/nitrojpeg.c`) önállóan dekódolja a
baseline és progresszív JPEG-eket (8 bit, Huffman, 1 vagy 3 komponens, YCbCr/szürke, bármilyen
mintavételezés, restart markerrel vagy anélkül; ~4100 letöltött JPEG-ből mindet). Ehhez csak C,
pthreads és AVX2 / NEON kell; a libjpeg-turbo a tesztek mércéje.

- **`TURBOJPEG=0`** (alapértelmezés): a maradékot (sérült és félbevágott fájlok, CMYK, RGB,
  aritmetikai kódolás, >16384 px) az Apple ImageIO dekódolja (ha az sem tudja, a címsorban
  `CANNOT DECODE` jelenik meg, és tovább lehet lapozni). Opcionális függőségek nélkül a nézőnek
  csak a macOS keretrendszerei kellenek.
- **`TURBOJPEG=1`**: ezeket a fájlokat előbb a TurboJPEG dekódolja (rajtuk kb. 2× gyorsabb az
  ImageIO-nál), 1.2 MB-tal nagyobb; a `--selftest` csak ebben a módban érhető el.

A nitrojpeg-ben a libjpeg-turbót igénylő részek `#ifdef NJ_REFERENCE` mögött vannak: a régi
motor, ahol a libjpeg-turbo dekódolja a sávokat, a CPU-s BGRX kimenet és az egyszálas
bejárás kísérlete. Ezeket csak a mérő- és tesztprogramok használják
(`build/nitrojpeg_ref.o`), a néző nem. A nézőben a tartalék dekóder `#ifdef NV_TURBOJPEG`
mögött van.

A libjpeg-turbo és a benchmark többi függősége (stb_image, Wuffs) letöltése és fordítása
a `third_party/` mappába. Semmit nem telepít a rendszerbe, kb. fél perc:

```
scripts/get-deps.sh
```

A mérő- és tesztprogramok a `samples/` mappában keresik a képeket, ide a saját JPEG-eidet
tedd (vagy symlinket). A mérések 50 saját fotón készültek, ezek nincsenek a repóban.

A `-march=native` miatt a bináris a fordító gép CPU-jára optimalizált.

## Fájlok

- `src/nitrojpeg.c/.h`: önálló párhuzamos JPEG-dekóder (saját Huffman + AVX2 IDCT);
  `NJ_REFERENCE`-szel a libjpeg-turbós összehasonlító részek is
- `src/nitroview.m`: Cocoa + Metal néző; `NV_TURBOJPEG`-gel TurboJPEG-tartalékkal
- `src/nitroview_win.c`: a néző Windowsra (Win32 + Direct3D 11 + WIC); `packaging/nitroview.ico/.rc`: az ikonja
- `src/gnome/`: GTK4/libadwaita néző GNOME/Linuxra; `packaging/gnome/`: asztali integráció
- `src/nitroview_sdl.c`: az SDL3 + OpenGL néző Linuxra (Wayland / X11)
- `src/libnitro.map`: a Linux megosztott könyvtár exportált API-ja
- `src/nitro_load.c/.h`: fájl -> dekódolt kép a fenti dekóderekkel (a könyvtár arca, ezt használják a nézők);
  `src/nitro_convert.c/.h`: a síkjaiból RGB; `examples/nitroconvert.c`: példaprogram a könyvtárra, bármilyen
  kép -> BMP; `src/nitro_prefetch.c/.h`, `src/nitro_view.h`: a nézők közös előtöltője és zoom-számításai
- `src/shaders.metal`: a néző GPU-shaderei (színkonverzió, kirajzolás), fordításkor beágyazva
- `scripts/get-deps.sh`: az opcionális függőségek letöltése és fordítása
- `bench/bench.m`: dekóder-benchmark (macOS, az összes dekóder), `bench/nbench.c`: hordozható
  benchmark a nitrojpeg / nitropng / nitropsd-hez, `bench/verify.c`: bitpontosság + szinkronstatisztika,
  `bench/robust.c`: hibatűrési teszt, `bench/freqprobe.c`: órajelmérés,
  `bench/sustain.c`: tartós terhelés, `bench/ab.sh`: zajtűrő A/B összehasonlítás
- `src/nitropng.c/.h`: önálló párhuzamos PNG-dekóder
- `src/nitropsd.c/.h`: párhuzamos PSD/PSB-dekóder (összefésült kép)
- `src/nitroheic.c/.h`: HEIC (HEIF) olvasása, a csempék párhuzamos kezelése
- `src/nitrohevc.c/.h`: az opcionális saját HEVC intra dekóderünk (csak `make NITROHEVC=1`-gyel, lásd fent: **HEVC**)
- `src/nitroavif.c/.h`: AVIF (AV1 a dav1d-vel, saját színkonverzió); `src/heif.h`: a HEIF-szerkezet,
  közös a nitroheic-kel
- `src/nitrotiff.c/.h`: TIFF-dekóder (csíkok / csempék párhuzamosan); `bench/bgrxverify.m`: az ő és a nitropng BGRX-kimenete az ImageIO-val összevetve
- `src/nitro_os.h`: amit a dekóderek a rendszerből használnak (párhuzamos ciklus, óra, magok száma)
- `src/png_wuffs.c/.h`: opcionális Wuffs-os PNG-dekódolás a nézőhöz (az interlaced PNG-khez, ezeket a nitropng kihagyja)
- `bench/pngbench.m`, `bench/pngsplit.c`: PNG-dekóderek összehasonlítása, időmegoszlás,
  `bench/pngverify.c`: a nitropng bájtpontossága a zlib-hez képest, `bench/pngrobust.c`: sérült PNG-k (ASan),
  `bench/mkpsd.py`: teszt-PSD-író (minden tömörítéssel), `bench/psdverify.c`: minden tömörítés a
  nyershez mérve, `bench/psdrobust.c`: sérült PSD-k (ASan)
- `bench/results/`: mért eredmények (`results.txt`, `ab_results.txt`, `matrix.txt`, `bands.txt`, `sync.txt`)

## További optimalizálási lehetőségek (mérve)

**A Huffman-táblák többnyire ugyanazok.** A fényképezőgépek és a telefonok a JPEG-szabvány
mintatábláit (Annex K) használják: mind a 43 Sony A6300-as és mind a 92 iPhone-os fotó, és a 178
Canon 50D-s fotóból 168 (a másik 10-et Photoshopban mentették újra, az optimalizált táblákat ír);
a webről származó 2341 JPEG-ből is 1506 (~2/3), a többinek többnyire saját, optimalizált táblája van.
A dekódolótáblák gyorsítótárazásával mégsem nyernénk semmit: a felépítésük képenként ~8 µs, a
dekódolás 0.1–0.4%-a. Az idő a táblák *használatában* megy el.

**Többszimbólumos táblák** (fix táblákkal megengedhetők, egyszer kell felépíteni): egyszálas
Huffman-dekódolás 27 Sony-fotón, ms/kép, minden kimenet azonos:

| keresőtábla | táblaméret | ms/kép |
|---|---|---|
| **1 szimbólum, 11 bit (nitrojpeg)** | 8 KB | **104.7** |
| 1 szimbólum, 12–16 bit | 16–256 KB | 105–141 |
| legfeljebb 3 szimbólum, 11–16 bit | 32 KB – 1 MB | 110–120 |
| 2 értékszimbólum, 11–14 bit, egyébként 1 | 16–128 KB | 103–105 (±1%) |

A 16 bites többszimbólumos tábla keresésenként 2.3 AC-szimbólumot ad, mégsem gyorsabb: a soros lánc
(a következő kód ott kezdődik, ahol az előző véget ért) megmarad, a nagyobb tábla pedig kiesik az L1
gyorsítótárból. Az AC-együtthatók adják a bitek 95.6%-át (ezekben a fotókban blokkonként 29.7 nem
nulla).

**A keresőtábla mérete a nitrojpeg-ben** (`LOOK`, bejegyzésenként 8 bájt, képenként 4 tábla), 43
Sony-fotó, két váltakozó futás átlaga, ms/kép:

| `LOOK` | táblák | párhuzamos (16 szál) | 1 szál |
|---|---|---|---|
| 8 | 8 KB | 11.0 | 110.0 |
| 9 | 16 KB | 10.6 | 106.5 |
| 10 | 32 KB | 10.0 | 98.9 |
| **11** | **64 KB** | **10.0** | **98.3** |
| 12 | 128 KB | 10.1 | 99.4 |
| 13 | 256 KB | 10.5 | 101.0 |
| 14 | 512 KB | 11.2 | 112.4 |
| 15 | 1 MB | 12.4 | 122.4 |
| 16 | 2 MB | 14.4 | 140.4 |

A 11 bit az optimum (10 és 12 között lapos): kevesebb bittel több kód (és kód + érték pár) nem fér
a keresésbe, és a lassú útra kerül; többel a blokkonként használt két tábla (DC + AC) nem fér el az
i9 48 KB-os L1 gyorsítótárában. A kimenet minden méretnél bitre azonos. Kisebb L1 gyorsítótárral
(Xeon E3-1245 v5, Skylake, 32 KB, Linux) a 10, 11 és 12 bit a mérési zajon belül egyforma (1 szálon
~174, ~171, ~173 ms/kép), így marad a 11.

Csapda ezen a Xeonon: néhány, a Huffman-kódot nem érintő módosítás után a párhuzamos dekódolás
34-ről 40–43 ms-ra lassult, és a 13 bit 18%-kal gyorsabbnak látszott. A kód elrendezése okozta,
nem a tábla: a Skylake-család processzorai (JCC-hiba miatti mikrokód) kiveszik a dekódolt uop-ok
gyorsítótárából azt a kódot, ahol egy ugrás átlóg egy 32 bájtos határon vagy ott végződik. A clang
`-mbranches-within-32B-boundaries` kapcsolójával (x86-on már benne van a Makefile-ban) minden
változat 34 ms 11 bittel (13 bittel 33, de az i9-en 3%-kal lassabb, ott a kapcsoló nem számít).

A legjobb méret a processzortól függ (L1 gyorsítótár, az ugrások elrendezése), ezért a saját
gépeden, főleg régebbin, érdemes a 11-es alapérték helyett 8-tól 16-ig végigmérni. A méret
fordításkor állítható, a kimenet mindegyikkel bitre azonos:

```sh
for L in 8 9 10 11 12 13 14 15 16; do make -B DEFS=-DLOOK=$L bench/nbench >/dev/null && printf "LOOK=$L " && bench/nbench -q -r 3 samples/*.jpg | tail -1; done
make -B DEFS=-DLOOK=13       # utána a nyertessel fordíts (a DEFS minden célba bekerül)
```

**A keresőtábla mérete a nitropng-ben** (literál/hossz-tábla: `LBITS`, távolságtábla: `DBITS`,
bejegyzésenként 4 bájt, a hosszabb kódok altáblákban), 34 valódi PNG, két váltakozó futás átlaga,
ms/kép:

| `LBITS` | `DBITS` | párhuzamos (16 szál) | 1 szál |
|---|---|---|---|
| 9 | 8 | 39.0 | 217.5 |
| 10 | 8 | 38.7 | 215.6 |
| **11** | **8** | **39.1** | **214.7** |
| 12 | 8 | 38.9 | 214.2 |
| 13 | 8 | 40.3 | 217.7 |
| 11 | 6 | 38.7 | 214.9 |
| 11 | 7 | 38.8 | 214.4 |
| 11 | 9 | 38.6 | 215.2 |
| 11 | 10 | 38.7 | 215.3 |

Mind a mérési zajon belül (±1.5%), csak a 13 bit kicsit lassabb; a kimenet minden méretnél bájtra
azonos. A JPEG-gel ellentétben itt a méret alig számít: a táblák kicsik (a 11 bites literál-tábla
8 KB), a gyakori deflate-kódok 11 bitnél rövidebbek, és az idő jó része a visszahivatkozások
másolására és a sorszűrők visszafejtésére megy el, amelyek nem függnek a tábláktól.

## Licenc

MIT, lásd [LICENSE](LICENSE). A `scripts/get-deps.sh` által letöltött opcionális függőségeknek
saját licencük van: libjpeg-turbo (BSD-szerű / IJG / zlib), Wuffs (Apache-2.0), stb_image
(public domain / MIT), SDL3 (zlib), dav1d (BSD-2-Clause), lunasvg és plutovg (MIT; a plutovg raszterizálója a FreeType
„smooth” raszterizálója, FreeType License: portions of this software are copyright © 2014 The
FreeType Project (www.freetype.org), all rights reserved), libcdr és librevenge (MPL-2.0; a libcdr-
módosításaink a github.com/gereoffy/libcdr-ben, a librevenge-é a patches/ mappában), lcms2 (MIT), boost (Boost Software License), mimalloc (MIT). Az IDCT a Independent JPEG Group libjpeg-jének jidctint.c
algoritmusát valósítja meg: ez a szoftver részben az Independent JPEG Group munkáján alapul.
