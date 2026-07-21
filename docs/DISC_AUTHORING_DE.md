# Dual-Layer- und BD-R-XL-Authoring-Erweiterungen für tsMuxeR

Dieser Fork ergänzt jaminmc/tsMuxer um Authoring-Funktionen für mehrschichtige Medien
(BD-R/RE DL und BD-R XL). Alle sind optional; das Standardverhalten bleibt unverändert.

## Neue Optionen (Mux-Modus)

| Option | Zweck |
|--------|-------|
| `--disc-size=<bd25\|bd50\|bd100\|bd128 \| Bytes>` | Passt-auf-die-Disc-Prüfung. Bricht vor dem Muxen ab, wenn das geschätzte Image nicht auf die Ziel-Disc passt. |
| `--allow-oversize` | Macht zusammen mit `--disc-size` aus einer Kapazitätsüberschreitung statt eines harten Fehlers nur eine Warnung. |
| `--layer-break-guard=<MB>` | Füllt `<MB>` nach jedem Layer-Break mit Nullen (der Anfang des nächsten Layers, wo Discs fehleranfällig sind), plus einen proportionalen Rand davor: ein Sechzehntel des Nachher-Werts (das 64:4-Verhältnis des ursprünglichen Entwurfs), mindestens 4 MB. 64 behält also die alten 4 MB davor, 288 ergibt 18 MB davor. Auf diesen Sektoren landen keine Dateidaten; die Datei, die den Break überquert (meist der Hauptfilm), bleibt eine logisch zusammenhängende UDF-Datei mit zwei Extents rund um die Lücke und spielt dank Read-Ahead nahtlos weiter. Die Zone ist absichtlich asymmetrisch: Tests auf echter Hardware zeigten, dass der Defekt des nächsten Layers etwa 35 MB erreichen kann, während das Ende des vorigen Layers sauber ist; das Budget geht also dorthin, wo der Defekt liegt. Verwende `288` (der Standard der grafischen Oberfläche: deckt die gemeldeten Defektzonen von 35 bis 258 MB und den verschobenen Break von Discs mit Fehlerverwaltung ab). `0` richtet nur aus, ohne Füllung. Ohne Angabe ist die Option aus. |
| `--layer-break-guard-before=<MB>` | Optional. Legt die Zone VOR jedem Break eigenständig fest, statt des proportionalen Standardrands. Der Standard ist asymmetrisch, weil der gemessene Defekt am Anfang des nächsten Layers sitzt, aber manche Medien sind auch kurz vor dem Break schwach; hiermit lassen sich beide Seiten auffüllen. Nicht setzen, um den asymmetrischen Standard zu behalten. |
| `--layer-break-lbn=<Sektor[,Sektor...]>` | Legt den bzw. die Layer-Break-Sektor(en) fest. Ein Wert für BD-R/RE DL, zwei für 100-GB-BD-R-XL, drei für 128-GB-BD-R-XL (siehe unten). |
| `--disc-capacity=<Sektoren>` | Optional, nur mit `--bdmv-to-iso`. Die gesamten Free Sectors der Disc; erlaubt der Layer-Fit-Platzierung zu prüfen, ob eine Datei, die ganz hinter einen Break verschoben wird, noch auf die Disc passt. Ohne Angabe wird aus dem Break-Abstand eine konservative Kapazität abgeleitet. Die grafische Oberfläche übergibt den Wert automatisch. |
| `--original-order` | Optional, nur mit `--bdmv-to-iso`. Schreibt die Dateien in ihrer numerischen (Wiedergabe-)Reihenfolge statt der größten zuerst; für Seamless-Branching-Discs, deren viele Segmente nahe an ihrer Wiedergabereihenfolge bleiben sollen. |
| `--no-layer-fit` | Optional, nur mit `--bdmv-to-iso`. Schaltet die Layer-Fit-Platzierung ab (siehe unten); die Datei, die die Schutzzone überquert, wird dann immer geteilt. |

## Neuer Modus: `--bdmv-to-iso`

```
tsMuxeR --bdmv-to-iso [--layer-break-guard=<MB>] [--layer-break-lbn=<sector[,sector...]>] <BDMV_folder> <out.iso>
```

Packt einen vorhandenen, unverschlüsselten BDMV-Ordner byte-genau in ein UDF-2.50-BD-ROM-ISO,
ohne neues Muxen und ohne Umnummerieren. Jede `.bdjo`-, `.jar`-, `.clpi`- und `.mpls`-Datei,
`index.bdmv`, `MovieObject.bdmv` und die `CERTIFICATE/`-Kette werden unverändert kopiert,
sodass BD-J-Menüs und alle Clip- und Playlist-Verweise gültig bleiben (die Schutzzone
verschiebt nur UDF-Extents, die das Dateisystem vor dem Player verbirgt). Die größte `.m2ts`
wird zuerst geschrieben, damit der Hauptfilm über den Layer-Break läuft und die Schutzzone
bekommt (oder verwende `--original-order`, siehe die Optionen oben). MakeMKV-Hilfsordner
werden übersprungen.

Layer-Fit-Platzierung (standardmäßig aktiv): Wenn die Datei, die eine Schutzzone überqueren
würde, vollständig zwischen das Ende dieser Zone und die nächste Zone passt (und in die
Disc-Kapazität), wird sie im Ganzen jenseits des Breaks platziert, statt geteilt zu werden.
Zwei typische Gewinne: Eine Disc mit zwei großen Titeln (Kinofassung und Director's Cut)
bekommt einen Titel pro Layer mit dem Break sauber dazwischen, und die vielen Segmente einer
Seamless-Branching-Disc ordnen sich so an, dass der Break zwischen Segmente fällt, genauso
wie kommerziell erstellte Discs ihn platzieren. Eine Datei, die größer als ein Layer ist,
überquert den Break weiterhin mit der Schutzzone darin, wie bisher.

Nach dem Bau melden das Log und eine Begleitdatei `<out.iso>.layerbreak.txt` die Position
jeder Schutzzone: den Sektorbereich der Nullen, die betroffene Datei, den Byte-Offset darin
und bei einer Stream-Datei die ungefähre Wiedergabezeit des Breaks. Beachte die im Abschnitt
zur grafischen Oberfläche beschriebene Seamless-Branching-Einschränkung: Innerhalb einer
Segmentdatei ist die Zeit relativ zum Segment.

Wiedergabeziel: Software-Player (VLC, libbluray, Kodi, PowerDVD) sind die zuverlässige
Umgebung für eine selbst erstellte BD-J-Disc. Manche Standalone-Player schränken BD-J auf
beschreibbaren Medien ein.

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
unverschlüsseltem Material: ein eingebundenes Kaufdisc-Image, das noch AACS trägt, ist so
nicht als normale Dateien lesbar. Diesen Schritt brauchst du nur, wenn du auf Dual-Layer-
oder BD-R-XL-Medien brennst und die Layer-Break-Schutzzone willst. Ein Single-Layer-Image,
das bereits läuft, kannst du direkt brennen.

## WICHTIG: Welche Zahlen gehören in `--layer-break-lbn`?

`--layer-break-lbn` ist der Sektor, an dem das Laufwerk physisch von einem Layer auf den
nächsten wechselt, in LBA-Sektoren zu 2048 Byte. Ein beschreibbarer BD-Rohling verteilt seine
Nutzkapazität gleichmäßig auf die Layer, also sind die Break-Sektoren einfache Bruchteile
der Gesamtsektoren der Disc:

```
BD-R/RE DL  (2 Layer): Gesamt / 2
BD-R XL 100 (3 Layer): Gesamt / 3  und  Gesamt * 2 / 3
BD-R XL 128 (4 Layer): Gesamt / 4,  Gesamt * 2 / 4,  Gesamt * 3 / 4
```

Für eine Standard-50-GB-BD-DL ist das `24.438.784 / 2 = 12.219.392` (25 GB pro Layer). Bei
einer 100-GB-BD-R-XL mit Gesamt `47.305.728` sind die beiden Breaks `15.768.576` und
`31.537.152`.

### Die Falle (unbedingt beachten)

Die Gesamtsektoren müssen aus der vollen formatierten Kapazität der Disc kommen, nicht aus
irgendeinem beliebigen API-Wert. Beispiel von einem echten Verbatim BD-R DL:

| Quelle | Gesamtsektoren | / 2 | richtig? |
|--------|----------------|-----|----------|
| ImgBurn „Disc Information, Free Sectors" | **24.438.784** | **12.219.392** | ja, der echte Break |
| Windows IMAPI `TotalSectorsOnMedia` | 23.652.352 | 11.826.176 | nein für eine BD-R DL: 23.652.352 ist die Kapazität mit Fehlerverwaltung (BD-RE DL), 0,8 GB zu früh |

Ein falscher (zu kleiner) Wert legt die Schutzzone etwa 0,8 GB vor den echten Layer-Wechsel.
Dann liegen doch Videodaten auf den fehleranfälligen Sektoren, also genau das, was die
Schutzzone verhindern soll. Verwende die volle formatierte Kapazität der Disc (ImgBurn
„Free Sectors" oder den formatierten bzw. maximalen Deskriptor von `READ FORMAT CAPACITIES`)
und teile sie durch die Layer-Anzahl.

Ein übergeordnetes Tool sollte diese Zahl vom Laufwerk lesen und an `--layer-break-lbn`
übergeben; tsMuxeR selbst spricht den Brenner nie an. Die grafische Oberfläche enthält einen
kleinen Rechner, der den ImgBurn-Wert „Free Sectors" für dich in die Break-Sektoren
umrechnet.

## Empfohlener Ablauf (mehrschichtige Disc mit Menüs und Schutzzone)

1. Lies die volle Kapazität der Ziel-Disc aus (zum Beispiel ImgBurn „Free Sectors"). Teile
   durch die Layer-Anzahl, um den bzw. die Break-Sektor(en) zu erhalten. Für eine
   Standard-50-GB-Disc ist das `12.219.392`.
2. `tsMuxeR --bdmv-to-iso --layer-break-guard=64 --layer-break-lbn=<Sektor[,Sektor...]> "<BDMV-Ordner>" Ausgabe.iso`
3. Brenne die `Ausgabe.iso` mit Verify. Der Layer-Break ist medienfest, der Brenner wechselt
   also am selben Sektor die Layer, und die Schutzzone landet exakt darauf.

## In der grafischen Oberfläche (der Reiter „BDMV folder -> ISO")

Die grafische Oberfläche bietet dieselben Funktionen ganz ohne Kommandozeile. Auf dem Reiter
„BDMV folder -> ISO":

- **BDMV folder and output ISO.** Wähle als Ordner das Wurzelverzeichnis der Disc, das
  `BDMV/` (und `CERTIFICATE/`) enthält. Ein eingebundenes ISO-Laufwerk lässt sich genauso
  als Ordner verwenden.
- **Disc-Typ mit vorausgefüllten Free Sectors.** Wähle die Disc aus der Liste (BD-R DL 50 GB,
  BD-RE DL 50 GB, BD-R XL 100 GB, BD-R XL 128 GB); die standardmäßigen „Free Sectors" werden
  automatisch eingetragen und **gesperrt**, sodass du ImgBurn nicht ausführen musst und den
  Wert nicht versehentlich änderst. Die Break-Sektoren werden für dich berechnet. BD-R DL und
  BD-RE DL sind getrennte Einträge, weil sie sich unterscheiden: eine leere BD-R DL fasst
  24.438.784 Sektoren (volle Kapazität), eine BD-RE DL dagegen 23.652.352 (sie reserviert
  Ersatzbereich für die Fehlerverwaltung). Ist deine Disc nicht standardmäßig (eine neu
  formatierte BD-RE oder eine ohne Fehlerverwaltung gebrannte BDXL), setze das Häkchen bei
  **Enter Free Sectors manually (advanced)**, um das Feld zu entsperren und den Wert
  einzugeben, den ImgBurn für deine konkrete Disc anzeigt.
- **Layer-break guard (after break).** Die Größe der Schutzzone in MB, mit einem farblich
  hervorgehobenen Hinweis. Der Standard ist 288 MB: gemeldete echte Defektzonen häufen sich
  um 35, 64 und 258 MB, und auf einer Disc mit Fehlerverwaltung kann der wahre Layer-Wechsel
  bis zu 128 MB hinter dem berechneten Break liegen; 288 deckt all das ab. Seltene Medien mit
  Defektzonen über 1 GB brauchen einen größeren Wert (das Feld akzeptiert bis zu 9999). Setze
  das Häkchen bei **Also fill before the break (advanced)**, um ein zweites Feld einzublenden
  und auch die Zone vor dem Break aufzufüllen, für Medien, die auf beiden Seiten des
  Übergangs schwach sind.
- **Keep original file order (seamless branching).** Normalerweise werden die Dateien mit der
  größten zuerst geschrieben, damit der Hauptfilm auf dem Layer-Break liegt und die
  Schutzzone bekommt. Discs mit Seamless Branching speichern den Film als viele
  Segmentdateien, die nacheinander abgespielt werden; mit diesem Häkchen bleiben die Dateien
  in ihrer numerischen (Wiedergabe-)Reihenfolge, sodass die Segmente physisch nah an der
  Reihenfolge liegen, in der sie abgespielt werden, wie auf der Original-Disc. In beiden Modi
  gilt: Passt die Datei, die den Break überqueren würde, vollständig auf den nächsten Layer,
  wird sie dort im Ganzen platziert; der Break fällt dann sauber zwischen zwei Dateien, statt
  eine zu teilen. Genauso platzieren auch kommerziell erstellte Discs ihre Layer-Breaks.
- **Layer-Break-Bericht.** Nach dem Bau zeigt das Log, wo jede Schutzzone gelandet ist:
  welche Stream-Datei, der Byte-Offset darin und (bei einem normalen Film aus einer einzigen
  Datei) die Wiedergabezeit, damit du weißt, wo du auf einem Player stichprobenartig prüfen
  kannst. Dieselben Informationen werden neben dem Image als `<name>.iso.layerbreak.txt`
  gespeichert. Aktuelle Einschränkung: Bei einer Seamless-Branching-Disc ist die angezeigte
  Zeit die Position innerhalb dieser Segmentdatei, nicht innerhalb des ganzen Films. Eine
  Zeit für den ganzen Film müsste den Playlists der Disc vertrauen, und viele Discs liefern
  Köder-Playlists aus; das Tool meldet daher das Segment und lässt dich die Stelle
  stattdessen über die Kapitel finden.
- **Fit estimate.** Eine Live-Anzeige zeigt die geschätzte Image-Größe im Verhältnis zur
  Disc-Kapazität an, während du Ordner, Disc-Typ, „Free Sectors" und Schutzzone einträgst:
  grün, wenn es passt (und wie viel Platz übrig bleibt), rot, wenn nicht (und um wie viel).
  Die Schutzzone fließt in die Schätzung ein; eine größere Zone aktualisiert die Anzeige also
  sofort.
- **Build ISO** führt denselben `--bdmv-to-iso`-Befehl mit diesen Werten aus.

## Seamless Branching: Was es ist und was es beeinflussen kann und was nicht

Auf der Disc ist ein Film mit Seamless Branching nicht eine Datei: Er besteht aus vielen
Segmentdateien (`00001.m2ts`, `00002.m2ts`, ...). Die Playlists sind kleine Skripte, die
sagen „spiele Segment 3, dann 7, dann 12"; die Kinofassung nimmt einen Pfad, der Director's
Cut einen anderen, wobei sie die meisten Segmente teilen. Das „Nahtlose" passiert zur
Wiedergabezeit im Puffer des Players. Es ist Choreographie, nichts, das in den Bytes anders
gespeichert wäre.

**Was das Branching beeinflusst: die Platzierung, nicht die Korrektheit.** Der Bau schreibt
die Dateien normalerweise mit der größten zuerst, auf einer Branching-Disc landen die
Segmente also nach Größe statt nach Wiedergabereihenfolge sortiert, was auf einem langsamen
Laufwerk etwas Suchweg kostet; genau dafür ist die Option „Keep original file order" da. Mit
der Layer-Fit-Platzierung fällt der Break so oder so zwischen zwei Segmente, dieselbe
Stelle, an der auch kommerzielles Authoring ihn platziert. Und die Wiedergabezeit im
Layer-Break-Bericht ist relativ zu der Segmentdatei, die er nennt, nicht zum ganzen Film
(siehe den Hinweis zum Bericht oben).

**Was das Branching nicht beeinflussen kann: die Verifikation.** Die Schutzzonen-Prüfung
liest rohe 2048-Byte-Sektoren an den Break-Positionen und stellt eine einzige Frage: Sind
diese Bytes null? Ob die umgebenden Daten zu einer großen Filmdatei gehören, zu Segment 47
einer Branching-Disc oder zur Lücke zwischen zwei Segmenten: Ein Nullband an einer
Disc-Position ist ein physischer Fakt, unabhängig von jeder darüberliegenden
Wiedergabelogik. Die Inhaltsprüfung hasht jede Datei auf beiden Seiten; eine Branching-Disc
hat schlicht mehr Dateien in der Liste, und die Playlist-Choreographie, die sie zur
Wiedergabezeit zusammenfügt, ändert kein einziges Byte in irgendeiner Datei. Wenn jedes
Segment identisch hasht, hat der Inhalt überlebt, in welcher Reihenfolge auch immer
irgendeine Fassung die Segmente abspielt.

Eine Analogie: Stell dir den Versand einer Kiste Bücher vor. Die Schutzzonen-Prüfung
bestätigt, dass das Schutzpolster an der richtigen Stelle in der Kiste sitzt, egal welche
Bücher es umgeben. Die Inhaltsprüfung vergleicht jedes Buch Seite für Seite mit der
ursprünglichen Versandliste. Seamless Branching ist die Tatsache, dass der Leser später in
einer Wähl-dein-eigenes-Abenteuer-Reihenfolge zwischen den Büchern springen wird; das ist
ein Leseverhalten, und es kann weder ändern, was in der Kiste ist, noch ob das Polster an
seinem Platz sitzt.

## Standard-Kapazitäten, die die Oberfläche vorausfüllt

Die Disc-Typ-Auswahl trägt diese „Free Sectors" einer leeren Disc ein (2048-Byte-Sektoren),
dieselben Zahlen, die ImgBurn für eine normale leere Disc anzeigt, und sperrt das Feld, damit
der Wert nicht versehentlich geändert wird:

| Disc | Free Sectors | Hinweis |
|------|-------------|---------|
| BD-R DL 50 GB | 24.438.784 | einmal beschreibbar, volle Kapazität |
| BD-RE DL 50 GB | 23.652.352 | wiederbeschreibbar, reserviert Ersatzbereich für die Fehlerverwaltung |
| BD-R XL 100 GB | 47.305.728 | BDXL, mit Fehlerverwaltung |
| BD-R XL 128 GB | 60.403.712 | BDXL, mit Fehlerverwaltung |

Das sind die (normalen) Kapazitäten mit Fehlerverwaltung. Die Zahlen sind Fakten über das
Medium: Sie wurden von echten Verbatim-Discs abgelesen (ImgBurn „Free Sectors") und stimmen
mit der Blu-ray/BDXL-Spezifikation überein. Eine OHNE Fehlerverwaltungs-Formatierung
gebrannte Disc hat eine größere volle Kapazität (zum Beispiel 48.878.592 bei 100 GB oder
62.500.864 bei 128 GB); zeigt ImgBurn eine davon für deine Disc an, setze das Häkchen bei
**Enter Free Sectors manually (advanced)** und gib sie ein. Besonders die BD-RE-Kapazität
hängt davon ab, wie die Disc formatiert wurde, weshalb sich das Feld entsperren lässt.

## Discs mit Fehlerverwaltung (auf echter Hardware gemessen)

Eine BD-R DL, die MIT Fehlerverwaltung (Ersatzbereichen) formatiert wurde, verhält sich
anders als eine ohne, und beide Effekte wurden auf einer echten gebrannten Disc gemessen,
nicht aus einem Datenblatt übernommen:

- **Kapazität.** Die Ersatzbereiche verringern die Free Sectors um exakt ihre Größe. ImgBurns
  Disc Definition Structure listet sie in Clustern zu 32 Sektoren; der `TDP`-Wert in ImgBurns
  Format Capacities ist die Summe der Ersatzbereiche (zum Beispiel `TDP: 24576` Cluster =
  786.432 Sektoren = die exakte Differenz zwischen 24.438.784 und 23.652.352). Eine BD-R DL
  mit Fehlerverwaltung nutzt daher den Kapazitätseintrag der BD-RE DL.
- **Der Layer-Wechsel liegt NICHT bei Kapazität geteilt durch Layer-Anzahl.** Die
  Ersatzbereiche können ungleich auf die Layer verteilt sein (eine echte ImgBurn-Formatierung
  ergab ISA0 4.096 + OSA0 6.144 auf Layer 0, aber ISA1 8.192 + OSA1 6.144 auf Layer 1). Jeder
  Layer fasst dann unterschiedlich viele Nutzdaten, und der wahre Wechsel liegt bei
  `Layer-Größe minus (ISA0+OSA0) x 32` Sektoren; auf einer echten Disc wurde er 128 MB NACH
  Kapazität/2 gemessen (ein Lesezeit-Scan zeigt die Refokus-Pause des Laufwerks am Übergang).
  BDXL-Formatierungen mit 100/128 GB haben symmetrische Ersatzbereiche pro Layer, dort stimmt
  die gleichmäßige Teilung exakt.
- **Was tun.** Nichts Besonderes: Die Standard-Schutzzone von 288 MB deckt den verschobenen
  Wechsel mit Reserve ab, und das wurde von Anfang bis Ende verifiziert (die Disc spielt
  nahtlos über den Übergang). Nur wenn du auf einer Disc mit Fehlerverwaltung eine kleine
  eigene Schutzzone verwendest, solltest du den Break mit `--layer-break-lbn` nach der Formel
  oben manuell setzen.
- **Kompromisse der Fehlerverwaltung bei Video.** Das Laufwerk verifiziert beim Schreiben und
  lagert schlechte Cluster selbstständig in die Ersatzbereiche um, was die Brenngeschwindigkeit
  ungefähr halbiert. Ein umgelagerter Cluster liegt physisch woanders; ihn während der
  Wiedergabe zu lesen erzwingt eine Kopfbewegung. Daten-Discs stört das nicht, aber für Video
  liefert ein normaler Brand (ohne Fehlerverwaltung) mit Schutzzone das flüssigere Ergebnis.
  Fehlerverwaltung und Schutzzone lösen verschiedene Probleme: Das Umlagern erhält die Daten,
  die Schutzzone hält das Bild ohne Kopfbewegungen am Laufen.

## Schutzzonen-Größe auf der gebrannten Disc (Sektor-Ausrichtung)

Die Schutzzone füllt immer ganze 2048-Byte-Sektoren und beginnt am Ende des letzten
Datenbereichs des Films, nicht an einem exakten Byte-Offset. Die Nullzone rund um den
Layer-Break richtet sich daher an Sektor- und Extent-Grenzen aus: Auf einer fertigen Disc
entspricht sie dem eingestellten Wert sehr genau (bis auf etwa 1 MB), aber nicht bis aufs
Byte, und sie fällt eher gleich groß oder etwas größer aus, nie nennenswert kleiner. Ein
eingestellter Wert von 160 MB pro Seite kann also als etwa 160,2 MB zurückgelesen werden.
Das ist normale Ausrichtung, kein Fehler. Da die Schutzzone in der Größenordnung von zig bis
hunderten MB liegt und den etwa 35 MB großen Defektbereich am Layer-Übergang abdeckt, hat
eine Abweichung von unter 1 MB keinen Einfluss auf die Wiedergabe. Wenn die Nullzone auf
einer fertigen Disc also nicht exakt dem eingegebenen Wert entspricht, ist das zu erwarten.

## BD-R XL (100 / 128 GB): Player-Kompatibilität

Viele Blu-ray-Player können 100- oder 128-GB-BD-R-XL-Discs gar nicht lesen, und es gibt
keine Garantie, dass ein bestimmter Player es kann. Wenn du das Image bei etwa 66 GB (den
ersten beiden Layern) hältst und die Disc finalisierst, steigen die Chancen bei manchen
Playern, aber auch 66 GB spielen nicht garantiert. Die vollen 100 oder 128 GB brauchen einen
aktuellen Player, der beschreibbare Medien mit hoher Kapazität ausdrücklich unterstützt.
Verbatim-Medien und langsames Brennen (2x) liefern die zuverlässigsten Ergebnisse. Das ist
eine Grenze des Players, nicht des ISO, also am besten am eigenen Gerät testen.
