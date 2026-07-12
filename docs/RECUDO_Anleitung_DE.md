# RECUDO – Komplette Blu-ray mit Menüs auf BD-R/RE DL (CLI-Anleitung)

Diese Erweiterung von tsMuxeR packt einen **bereits entschlüsselten** BDMV-Ordner
1:1 in ein brennfertiges BD-ROM-ISO – **mit erhaltenen BD-J-Menüs** und einer
**Layer-Break-Schutzzone** für Dual-Layer-Rohlinge.

> Hinweis: Das Entschlüsseln der Disc ist Sache des Nutzers und nicht Teil dieses Tools.

---

## Was macht es genau?

- Kopiert den kompletten BDMV-Ordner (z. B. ein MakeMKV-Rip) **byte-genau** in ein
  UDF-2.50-BD-ROM-ISO. Es wird **nicht neu gemuxt und nichts umnummeriert**, daher
  bleiben **BD-J-Menüs, Wiedergabelisten und alle Clip-Verweise gültig** (auch die
  Signaturen in `CERTIFICATE/`).
- **Layer-Break-Schutzzone:** Bei einer Dual-Layer-Disc wechselt der Brenner an einem
  festen Punkt (der „Layer Break") von Layer 0 auf Layer 1. Die ersten Sektoren des
  zweiten Layers sind bei günstigen Rohlingen oft fehleranfällig. Das Tool füllt eine
  Zone rund um diesen Punkt mit Nullen, damit dort **keine Videodaten** liegen. Die
  betroffene Datei (meist der Hauptfilm) bleibt logisch zusammenhängend – die Wiedergabe
  läuft **nahtlos** über den Layer-Wechsel (der Player-Puffer überbrückt die Lücke).

---

## Voraussetzungen

- Entschlüsselter BDMV-Ordner (Struktur: `BDMV/`, `CERTIFICATE/` …).
- `tsMuxeR.exe` (dieser Fork).
- Zum Brennen: **ImgBurn** + **BD-R/RE DL** Rohling + DL-fähiger Brenner.

---

## Schritt 1 – Layer-Break der Disc ermitteln (WICHTIG!)

Der Wert für `--layer-break-lbn` ist die **Kapazität von Layer 0 in Sektoren**
(1 Sektor = 2048 Byte). Bei BD-R/RE DL sind beide Layer gleich groß, also:

```
layer-break-lbn = Gesamtsektoren der Disc / 2
```

- **Standard-50-GB-DL:** `24.438.784 / 2 = 12.219.392` (= 25 GB pro Layer).
  Das ist der **Standardwert** – bei normalen 50-GB-Rohlingen musst du
  `--layer-break-lbn` gar nicht angeben.

### ⚠️ Die Falle (unbedingt beachten)

Die Gesamtsektoren müssen aus der **vollen formatierten Kapazität** kommen, nicht aus
irgendeinem beliebigen API-Wert. Beispiel von einem echten Verbatim BD-R DL:

| Quelle | Gesamtsektoren | /2 | richtig? |
|--------|----------------|-----|----------|
| ImgBurn: „Disc Information → **Free Sectors**" | **24.438.784** | **12.219.392** | ✅ echter Break |
| Windows/IMAPI `TotalSectorsOnMedia` | 23.652.352 | 11.826.176 | ❌ nur ein Teil-Kapazitätswert – **0,8 GB zu früh** |

Ein falscher (zu kleiner) Wert legt die Schutzzone ~0,8 GB **vor** den echten
Layer-Wechsel – dann liegen doch Videodaten auf den kritischen Sektoren. **Immer die
volle formatierte Kapazität nehmen** (in ImgBurn „Free Sectors") und durch 2 teilen.

**So liest du sie in ImgBurn ab:** Disc einlegen → rechts im Feld „Device / Media
Information" die Zeile **Free Sectors** ablesen → durch 2 teilen.

---

## Schritt 2 – ISO bauen

```
tsMuxeR --bdmv-to-iso --layer-break-guard=64 [--layer-break-lbn=<Sektor>] "<BDMV-Ordner>" "<Ausgabe.iso>"
```

- `--layer-break-guard=64` = 64 MB Nullen **nach** dem Break (Layer-1-Start, dort sind Discs
  fehleranfällig) plus 4 MB Rand davor. Die Zone ist **asymmetrisch**, weil der Defekt am
  Layer-1-Anfang liegt (real gemessen ~35 MB, Layer 0 war sauber) - **64 empfohlen**.
- `--layer-break-lbn=<Sektor>` = nur nötig, wenn **kein** Standard-50-GB-Rohling
  (sonst weglassen → automatisch 12.219.392).

**Beispiel (Standard-50-GB-Disc):**
```
tsMuxeR --bdmv-to-iso --layer-break-guard=64 "D:\ZurueckInDieZukunft\BDMV_Ordner" "D:\BTTF.iso"
```

**Beispiel (Disc mit abweichender Kapazität, Break in ImgBurn ermittelt):**
```
tsMuxeR --bdmv-to-iso --layer-break-guard=64 --layer-break-lbn=11826176 "D:\Film\BDMV_Ordner" "D:\Film.iso"
```

Der Ordner muss den `BDMV`-Ordner (und ggf. `CERTIFICATE`) enthalten. MakeMKV-Hilfsordner
werden automatisch übersprungen. Der größte `.m2ts` (der Hauptfilm) wird zuerst
geschrieben, damit er über den Layer-Break läuft und die Schutzzone bekommt.

---

## Schritt 3 – Brennen (ImgBurn)

1. ImgBurn öffnen → Modus **Write** (Schreiben).
2. Als Quelle das erstellte `.iso` wählen, als Ziel den DL-Brenner.
3. **Verify (Überprüfen) einschalten**, Geschwindigkeit z. B. 4x.
4. Brennen.

Der Layer-Break ist bei BD-R/RE DL **medienfest** – der Brenner wechselt genau am
selben Sektor, für den die ISO gebaut wurde. Die Schutzzone landet also exakt auf dem
physischen Layer-Übergang.

---

## Optional – Passt alles auf die Disc?

```
--disc-size=bd50
```
bricht vor dem Bau ab, wenn das Image nicht auf die Zielgröße passt
(Werte: `bd25`, `bd50`, `bd100`, `bd128`). Mit `--allow-oversize` nur Warnung statt Abbruch.

---

## Wiedergabe

- **Zuverlässig:** Software-Player (VLC, Kodi, PowerDVD, libbluray) für selbst erstellte
  BD-J-Discs.
- **Standalone-Player:** Manche spielen BD-J von **selbst gebrannten** Rohlingen nur
  eingeschränkt ab – das liegt am Player, nicht am ISO. Am besten am eigenen Gerät testen.

## Einschränkungen

- Menüs werden **erhalten**, nicht bearbeitet (keine Änderung der Navigation).
- Es wird **nichts neu gemuxt** – dadurch bleiben alle Menü- und Clip-Verweise gültig.
- Nur für Dual-Layer sinnvoll (Single-Layer hat keinen Layer-Break).

---

## Kurzreferenz

| Option | Bedeutung |
|--------|-----------|
| `--bdmv-to-iso <Ordner> <iso>` | BDMV-Ordner 1:1 in ein BD-ROM-ISO packen (Menüs bleiben) |
| `--layer-break-guard=<MB>` | Nullen in MB **nach** dem Break (Layer-1-Seite) + 4 MB davor; 64 empfohlen |
| `--layer-break-lbn=<Sektor>` | Layer-0-Kapazität in Sektoren (= Gesamtsektoren / 2); Standard 12.219.392 |
| `--disc-size=bd25\|bd50\|bd100\|bd128` | Abbruch, falls Image nicht passt |
| `--allow-oversize` | mit `--disc-size`: nur Warnung statt Abbruch |
