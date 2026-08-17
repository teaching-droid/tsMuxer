# Dolby Vision

tsMuxeR kann eine Dual-Layer-Disc mit Dolby Vision in einer einzigen Matroska-Datei ablegen und
die Disc aus dieser Datei wieder erstellen. Diese Seite erklärt, was die Optionen bewirken und was
dabei erhalten bleibt und was nicht.

Aktualisiert bis 2.18.

## Wie eine Dual-Layer-Disc mit Dolby Vision aufgebaut ist

Eine solche Disc verteilt das Bild auf **zwei** Videostreams:

- einen **Base Layer**, das Bild in voller Auflösung, das ganz gewöhnliches HDR10 ist
- einen **Enhancement Layer**, einen Stream in Viertelauflösung, der die Dolby-Vision-Daten trägt,
  zusammen mit einer RPU pro Bild

Beide werden gebraucht. Ein Player, der Dolby Vision versteht, liest sie gemeinsam; ein Player, der
es nicht versteht, zeigt den Base Layer als HDR10.

Matroska hat für einen zweiten Videostream dieser Art keinen Platz. Eine Datei mit zwei Videospuren
ist nicht das, was ein Player erwartet, und kein Player schaltet daraus Dolby Vision ein. tsMuxeR
führt das Ganze deshalb in **einer** Spur mit: den Base Layer, den Enhancement Layer so verpackt,
dass ein Decoder, der nichts von Dolby Vision weiß, ihn überspringt, und die RPU. Nichts wird neu
codiert und nichts umsortiert.

## Eine Disc als Matroska ablegen

Fügen Sie die Disc oder ihre beiden Videostreams hinzu, wählen Sie **Muxen in MKV** und muxen Sie.
Die beiden Videostreams werden als die zwei Layer eines Bildes erkannt und automatisch
zusammengeführt.

## Die Disc wieder erstellen

Öffnen Sie die Matroska-Datei. Eine zusammengeführte Dolby-Vision-Spur erscheint als **zwei
Zeilen**, eine mit `(base layer)` und eine mit `(enhancement layer)` gekennzeichnet. Markieren Sie
beide, wählen Sie eine Blu-ray-Ausgabe und muxen Sie. Die Layer werden wieder getrennt und landen
auf den richtigen Streams.

Auf der Kommandozeile wird dasselbe als zwei Zeilen mit `subTrack=` geschrieben:

```
V_MPEGH/ISO/HEVC, "film.mkv", track=1, subTrack=1, fps=23.976
V_MPEGH/ISO/HEVC, "film.mkv", track=1, subTrack=2, fps=23.976
```

`subTrack=1` ist der Base Layer und `subTrack=2` der Enhancement Layer.

## Das Dolby-Vision-Profil

Eine Dual-Layer-Disc ist **Profil 7**. Viele Geräte akzeptieren Profil 7 aus einer Datei nicht,
obwohl sie dieselbe Disc anstandslos abspielen. **Profil 8.1** ist eine Single-Layer-Form, die weit
mehr Geräte akzeptieren und die überall sonst auf HDR10 zurückfällt.

Wenn die Quelle eine Dual-Layer-Disc mit Dolby Vision ist und die Ausgabe Matroska, erscheint neben
dem Ausgabeformat eine Auswahl **Dolby Vision**:

| Auswahl | was sie bewirkt |
|---|---|
| Profil 7, wie auf der Disc | übernimmt die Disc unverändert. Das ist die Voreinstellung. |
| Profil 8.1, läuft auf mehr Geräten | wandelt die Metadaten so um, dass die Datei als Single-Layer-Dolby-Vision läuft |

Auf der Kommandozeile ist das `--dv-profile=7` oder `--dv-profile=8.1`.

### Profil 8.1 macht die Datei nicht kleiner

Das ist der Punkt, der überrascht. Der übliche Weg, eine Disc nach Profil 8.1 zu wandeln, wirft den
Enhancement Layer weg, und genau daher kommt die Platzersparnis.

tsMuxeR wirft ihn nicht weg. Der Enhancement Layer wird weiterhin in der Spur mitgeführt, und die
**eigenen** Dolby-Vision-Metadaten der Disc sind daneben angehängt, sodass sich die ursprüngliche
Disc exakt wieder erstellen lässt. Eine gewandelte Datei ist deshalb ungefähr so groß wie eine mit
Profil 7 und kann sogar etwas größer ausfallen.

Wenn Sie eine kleine Profil-8.1-Datei wollen und die Disc nicht wieder erstellen möchten, ist dies
nicht das richtige Werkzeug dafür.

### Warum die ursprünglichen Metadaten aufbewahrt werden müssen

Die Umwandlung von Profil-7-Metadaten nach 8.1 ist **viele zu eins**: Verschiedene Originale
ergeben dasselbe Ergebnis. Auf einer gepressten Disc gemessen, wurden zwei Drittel der gewandelten
Metadaten ununterscheidbar, wobei bis zu 91 verschiedene Originale auf einem einzigen Ergebnis
landeten. Das lässt sich hinterher durch nichts rückgängig machen, wie gut eine Umsetzung auch sein
mag.

Die Originale werden deshalb unverändert aufbewahrt statt rekonstruiert. Genau das macht den Hin-
und Rückweg möglich, und genau deshalb schrumpft die Datei nicht.

### Was Profil 8.1 braucht

Profil 8.1 nutzt die Bibliothek **libdovi**, die nicht Teil von tsMuxeR ist.

| Plattform | was zu tun ist |
|---|---|
| Windows 64-Bit | `dovi.dll` neben `tsMuxeR.exe` legen. Eine fertig gebaute Version wird veröffentlicht. |
| Linux, macOS | libdovi aus dem Quelltext bauen oder ein Paket installieren, falls Ihre Distribution eines hat |
| Windows 32-Bit | nicht verfügbar. Es gibt keinen 32-Bit-Build der Bibliothek. |

Fehlt die Bibliothek, wird `--dv-profile=8.1` **vor** dem Start des Muxens mit einer entsprechenden
Meldung abgelehnt, statt mitten in einem Spielfilm zu scheitern.

## Was erhalten bleibt

Gemessen wurde an ganzen Titeln statt an kurzen Clips, denn ein Clip kann weder das Ende eines
Streams noch eine Naht zwischen Segmenten prüfen, und beides erwies sich als wichtig:

- ein ganzer Spielfilm, beide Layer: alle 2.064.653 Einheiten identisch und alle 166.928
  ursprünglichen Metadatensätze der Disc wiederhergestellt
- ein ganzes Segment, beide Layer: jede Einheit identisch und über 7 GB hinweg Byte für Byte
  identisch
- ein ganzer Titel mit **Seamless Branching**, 23 an 22 Stellen aneinandergefügte Clips: der
  Neuaufbau mit Profil 7 und der mit Profil 8.1 sind auf beiden Layern Byte für Byte identisch

Im Klartext: **Jedes Bild und jedes Stück Dolby-Vision-Metadaten kommt exakt zurück.**

### Eine ehrliche Einschränkung

Ein neu aufgebauter Stream ist überall dort Byte für Byte mit der Quelle identisch, wo die Disc
dieselbe Form von Startcode verwendet, die auch tsMuxeR schreibt, und das trifft auf die meisten
Discs zu, aber nicht auf alle. Wo eine Disc abweicht, liegt der Unterschied allein in der Länge des
Startcodes, die in beiden Formen zulässig ist und identisch decodiert wird; Bilder und Metadaten
bleiben davon unberührt. Das ist keine Besonderheit von Dolby Vision und gilt für Profil 7 genauso.

Audio, Untertitel und die Disc-Struktur werden neu erstellt, wie bei jedem Muxen. Die Aussage oben
bezieht sich auf das Video.

### Ablehnen statt raten

Eine Datei, deren aufbewahrte Metadaten nicht zu dem passen, was sie angibt, wird **abgelehnt**,
bevor irgendetwas geschrieben wird, statt eine Disc zu erzeugen, die richtig aussieht und falsch
ist. Das gilt für eine Datei, die geschnitten, neu gemuxt oder von einem anderen Werkzeug um ihre
Anhänge gebracht wurde.

Wurde eine Datei von tsMuxeR geschrieben und dann von einer anderen Software kopiert, die die
Anhänge verworfen hat, sind die Dolby-Vision-Daten weg, die zum Wiederaufbau der Disc nötig sind.
Bewahren Sie das Original auf.

## Eine Single-Layer-Datei mit Dolby Vision

Eine Datei, die bereits Single Layer ist, Profil 5 oder Profil 8, hat keinen Enhancement Layer, den
man abtrennen könnte. Eine angeforderte Aufteilung wird mit einer Erklärung abgelehnt, statt eine
leere zweite Spur zu erzeugen. Muxen Sie sie stattdessen als eine einzige Spur.
