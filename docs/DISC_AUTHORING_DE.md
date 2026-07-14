# Komplette Blu-ray mit Menüs auf BD-R/RE DL und BD-R XL (CLI-Anleitung)

Diese Erweiterung von tsMuxeR packt einen **unverschlüsselten** BDMV-Ordner
1:1 in ein brennfertiges BD-ROM-ISO. Die **BD-J-Menüs bleiben erhalten**, und eine
**Layer-Break-Schutzzone** schützt die Übergänge bei Dual-Layer- und BD-R-XL-Rohlingen.

> Hinweis: Das ISO wird aus einem bereits lesbaren, unverschlüsselten BDMV-Ordner gebaut.
> Das Tool selbst ver- oder entschlüsselt nichts.

---

## Was macht es genau?

- Kopiert den kompletten BDMV-Ordner (z. B. ein MakeMKV-Rip) **byte-genau** in ein
  UDF-2.50-BD-ROM-ISO. Es wird **nicht neu gemuxt und nichts umnummeriert**, daher
  bleiben **BD-J-Menüs, Wiedergabelisten und alle Clip-Verweise gültig** (auch die
  Signaturen in `CERTIFICATE/`).
- **Layer-Break-Schutzzone:** Bei einer mehrschichtigen Disc wechselt der Brenner an
  festen Punkten (den „Layer Breaks") von einem Layer auf den nächsten. Die ersten
  Sektoren des nächsten Layers sind bei günstigen Rohlingen oft fehleranfällig. Das Tool
  füllt eine Zone rund um jeden dieser Punkte mit Nullen, damit dort **keine Videodaten**
  liegen. Die betroffene Datei (meist der Hauptfilm) bleibt logisch zusammenhängend, und
  die Wiedergabe läuft **nahtlos** über den Layer-Wechsel (der Player-Puffer überbrückt
  die Lücke).

---

## Voraussetzungen

- Unverschlüsselter BDMV-Ordner (Struktur: `BDMV/`, `CERTIFICATE/` usw.).
- `tsMuxeR.exe` (dieser Fork).
- Zum Brennen: **ImgBurn**, ein passender Rohling (BD-R/RE DL oder BD-R XL) und ein
  Brenner, der das Medium unterstützt.

---

## Aus einem vorhandenen ISO arbeiten

tsMuxeR liest einen BDMV-Ordner, kein ISO-Image, und bearbeitet ein ISO nie direkt. Du musst
das ISO aber nicht vorher entpacken. Binde es in einem virtuellen Laufwerk ein, sodass es als
Laufwerksbuchstabe erscheint, und richte `--bdmv-to-iso` dann auf dieses Laufwerk. tsMuxeR
liest die Disc-Struktur direkt aus dem eingebundenen Image:

- Windows 8, 10 und 11: das `.iso` doppelklicken, um es einzubinden (z. B. als `E:`).
- Windows 7: ein kostenloses Tool für virtuelle Laufwerke installieren (WinCDEmu oder
  Virtual CloneDrive), dann einbinden.

```
tsMuxeR --bdmv-to-iso --layer-break-guard=64 --layer-break-lbn=<Sektor[,Sektor...]> E:\ Ausgabe.iso
```

tsMuxeR liest die Dateien vom eingebundenen Laufwerk und schreibt in einem Durchgang ein
neues ISO mit Schutzzone, byte-genau und ohne neues Muxen. Das funktioniert nur mit
**unverschlüsseltem** Material: ein eingebundenes Kaufdisc-Image, das noch AACS trägt, ist so
nicht als normale Dateien lesbar. Diesen Schritt brauchst du nur, wenn du auf Dual-Layer- oder
BD-R-XL-Medien brennst und die Layer-Break-Schutzzone willst. Ein Single-Layer-Image, das
bereits läuft, kannst du direkt brennen.

---

## Schritt 1: Layer-Break der Disc ermitteln (WICHTIG!)

Der Wert für `--layer-break-lbn` ist der Sektor, an dem der Brenner physisch von einem
Layer auf den nächsten wechselt (1 Sektor = 2048 Byte). Ein Rohling verteilt seine
Nutzkapazität gleichmäßig auf die Layer, also sind die Break-Sektoren einfache Bruchteile
der Gesamtsektoren:

```
BD-R/RE DL  (2 Layer): Gesamt / 2
BD-R XL 100 (3 Layer): Gesamt / 3  und  Gesamt * 2 / 3
BD-R XL 128 (4 Layer): Gesamt / 4,  Gesamt * 2 / 4,  Gesamt * 3 / 4
```

Beispiele:

- **Standard-50-GB-DL:** `24.438.784 / 2 = 12.219.392` (25 GB pro Layer).
- **100-GB-BD-R-XL** mit Gesamt `47.305.728`: die beiden Breaks sind `15.768.576` und
  `31.537.152`.

### ⚠️ Die Falle (unbedingt beachten)

Die Gesamtsektoren müssen aus der **vollen formatierten Kapazität** kommen, nicht aus
irgendeinem beliebigen API-Wert. Beispiel von einem echten Verbatim BD-R DL:

| Quelle | Gesamtsektoren | / 2 | richtig? |
|--------|----------------|-----|----------|
| ImgBurn: „Disc Information, **Free Sectors**" | **24.438.784** | **12.219.392** | ✅ echter Break |
| Windows/IMAPI `TotalSectorsOnMedia` | 23.652.352 | 11.826.176 | ❌ nur ein Teil-Kapazitätswert, 0,8 GB zu früh |

Ein falscher (zu kleiner) Wert legt die Schutzzone etwa 0,8 GB **vor** den echten
Layer-Wechsel. Dann liegen doch Videodaten auf den kritischen Sektoren. **Immer die volle
formatierte Kapazität nehmen** (in ImgBurn „Free Sectors") und durch die Layer-Anzahl teilen.

**So liest du sie in ImgBurn ab:** Disc einlegen, dann rechts im Feld „Device / Disc
Information" die Zeile **Free Sectors** ablesen. Die Zeile „Number of Layers" darunter zeigt
50, 100 oder 128 GB an.

> Tipp: In der grafischen Oberfläche gibt es dafür einen kleinen Rechner. Du gibst nur die
> „Free Sectors" ein und wählst den Disc-Typ, die Break-Sektoren werden automatisch berechnet.

---

## Schritt 2: ISO bauen

```
tsMuxeR --bdmv-to-iso --layer-break-guard=64 --layer-break-lbn=<Sektor[,Sektor...]> "<BDMV-Ordner>" "<Ausgabe.iso>"
```

- `--layer-break-guard=64` bedeutet 64 MB Nullen **nach** jedem Break (Anfang des nächsten
  Layers, dort sind Discs fehleranfällig) plus 4 MB Rand davor. Die Zone ist
  **asymmetrisch**, weil der Defekt am Layer-Anfang liegt (real gemessen etwa 35 MB, der
  vorige Layer war sauber). **64 empfohlen.**
- `--layer-break-lbn` bekommt bei BD-R/RE DL einen Wert, bei 100-GB-BD-R-XL zwei Werte
  (durch Komma getrennt) und bei 128-GB-BD-R-XL drei Werte.

**Beispiel (50-GB-DL, Break in ImgBurn ermittelt):**
```
tsMuxeR --bdmv-to-iso --layer-break-guard=64 --layer-break-lbn=12219392 "D:\ZurueckInDieZukunft\BDMV_Ordner" "D:\BTTF.iso"
```

**Beispiel (100-GB-BD-R-XL, zwei Breaks):**
```
tsMuxeR --bdmv-to-iso --layer-break-guard=64 --layer-break-lbn=15768576,31537152 "D:\Film\BDMV_Ordner" "D:\Film.iso"
```

Der Ordner muss den `BDMV`-Ordner (und ggf. `CERTIFICATE`) enthalten. MakeMKV-Hilfsordner
werden automatisch übersprungen. Der größte `.m2ts` (der Hauptfilm) wird zuerst
geschrieben, damit er über den Layer-Break läuft und die Schutzzone bekommt.

---

## Schritt 3: Brennen (ImgBurn)

1. ImgBurn öffnen, Modus **Write** (Schreiben).
2. Als Quelle das erstellte `.iso` wählen, als Ziel den Brenner.
3. **Verify (Überprüfen) einschalten**, Geschwindigkeit z. B. 4x (bei BD-R XL lieber 2x).
4. Brennen.

Der Layer-Break ist medienfest. Der Brenner wechselt genau an demselben Sektor, für den
die ISO gebaut wurde. Die Schutzzone landet also exakt auf dem physischen Layer-Übergang.

---

## Optional: Passt alles auf die Disc?

```
--disc-size=bd50
```
bricht vor dem Bau ab, wenn das Image nicht auf die Zielgröße passt
(Werte: `bd25`, `bd50`, `bd100`, `bd128`). Mit `--allow-oversize` gibt es nur eine Warnung
statt eines Abbruchs.

---

## In der grafischen Oberfläche (der Reiter „BDMV folder -> ISO")

Die grafische Oberfläche bietet dieselben Funktionen ganz ohne Kommandozeile. Auf dem Reiter „BDMV folder -> ISO":

- **BDMV folder and output ISO.** Wähle als Ordner das Wurzelverzeichnis der Disc, das `BDMV/` (und `CERTIFICATE/`) enthält. Ein eingebundenes ISO-Laufwerk lässt sich genauso als Ordner verwenden.
- **Layer-break calculator.** Wähle den Disc-Typ und füge die „Free Sectors" aus ImgBurn ein. Die Break-Sektoren werden automatisch berechnet; passt der Wert nicht zum gewählten Disc-Typ, erscheint eine Warnung.
- **Layer-break guard (after break).** Hier legst du die Größe der Schutzzone in MB fest; ein farblich hervorgehobener Hinweis nennt den empfohlenen Wert (64 MB). Setze das Häkchen bei **Also fill before the break (advanced)**, um ein zweites Feld einzublenden und auch die Zone vor dem Break aufzufüllen. Das hilft bei Medien, die auf beiden Seiten des Übergangs schwach sind.
- **Fit estimate.** Eine Live-Anzeige zeigt die geschätzte Image-Größe im Verhältnis zur Disc-Kapazität an, während du Ordner, Disc-Typ, „Free Sectors" und Schutzzone einträgst: grün, wenn es passt (und wie viel Platz übrig bleibt), rot, wenn nicht (und um wie viel). Die Schutzzone fließt in die Schätzung ein; eine größere Zone aktualisiert die Anzeige also sofort.
- **Build ISO** führt denselben `--bdmv-to-iso`-Befehl mit diesen Werten aus.

---

## BD-R XL (100 / 128 GB): Wiedergabe im Player

Viele Blu-ray-Player können 100- oder 128-GB-BD-R-XL-Discs **gar nicht** lesen, und es gibt
keine Garantie, dass ein bestimmter Player es kann. Wenn du das Image bei etwa 66 GB (den
ersten beiden Layern) hältst und die Disc finalisierst, steigen die Chancen bei manchen
Playern, aber auch 66 GB spielen nicht garantiert. Die vollen 100 oder 128 GB brauchen einen
aktuellen Player, der beschreibbare Medien mit hoher Kapazität ausdrücklich unterstützt.
Verbatim-Medien und langsames Brennen (2x) liefern die zuverlässigsten Ergebnisse. Das ist
eine Grenze des Players, nicht des ISO, also am besten am eigenen Gerät testen.

---

## Wiedergabe allgemein

- **Zuverlässig:** Software-Player (VLC, Kodi, PowerDVD, libbluray) für selbst erstellte
  BD-J-Discs.
- **Standalone-Player:** Manche spielen BD-J von **selbst gebrannten** Rohlingen nur
  eingeschränkt ab. Das liegt am Player, nicht am ISO. Am besten am eigenen Gerät testen.

## Einschränkungen

- Menüs werden **erhalten**, nicht bearbeitet (keine Änderung der Navigation).
- Es wird **nichts neu gemuxt**, dadurch bleiben alle Menü- und Clip-Verweise gültig.
- Nur für mehrschichtige Discs sinnvoll (Single-Layer hat keinen Layer-Break).

---

## Kurzreferenz

| Option | Bedeutung |
|--------|-----------|
| `--bdmv-to-iso <Ordner> <iso>` | BDMV-Ordner 1:1 in ein BD-ROM-ISO packen (Menüs bleiben) |
| `--layer-break-guard=<MB>` | Nullen in MB **nach** jedem Break (Layer-Seite) plus 4 MB davor; 64 empfohlen |
| `--layer-break-guard-before=<MB>` | Optional: Zone **vor** dem Break eigenständig festlegen statt der 4 MB Standardmarge; für Medien, die auch vor dem Break schwach sind |
| `--layer-break-lbn=<Sektor[,Sektor...]>` | Break-Sektor(en): 1 Wert bei DL, 2 bei 100 GB, 3 bei 128 GB |
| `--disc-size=bd25\|bd50\|bd100\|bd128` | Abbruch, falls Image nicht passt |
| `--allow-oversize` | mit `--disc-size`: nur Warnung statt Abbruch |
