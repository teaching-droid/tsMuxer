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

**Woher man sie bekommt.** Die Bibliothek wird auf der
[dovi_tool Release-Seite](https://github.com/quietvoid/dovi_tool/releases) veröffentlicht, als
`libdovi-<Version>-x86_64-pc-windows-msvc.zip`. In diesem Archiv liegt genau eine Datei,
`dovi.dll`, und das ist die Datei, die neben `tsMuxeR.exe` gehört.

Nehmen Sie nicht `dovi_tool-<Version>-x86_64-pc-windows-msvc.zip` von derselben Seite. Das ist
das Kommandozeilenprogramm, damit kann tsMuxeR nichts anfangen. Beide liegen nebeneinander, und
das Programm hat den naheliegenderen Namen. Genau das ist der übliche Irrtum.

Die Release-Nummer ist die von dovi_tool, die Bibliothek darin trägt eine andere:
`libdovi-3.4.0` hängt am Release `2.3.3`. Wer auf der Seite nach der Versionsnummer der
Bibliothek sucht, findet nichts. Jede libdovi ab 3.3.1 enthält die vier Einsprungpunkte, die
tsMuxeR benötigt.

Fehlt die Bibliothek, wird `--dv-profile=8.1` **vor** dem Start des Muxens mit einer entsprechenden
Meldung abgelehnt, statt mitten in einem Spielfilm zu scheitern.

## Was erhalten bleibt

Gemessen wurde an ganzen Titeln statt an kurzen Clips, denn ein Clip kann weder das Ende eines
Streams noch eine Naht zwischen Segmenten prüfen, und beides erwies sich als wichtig:

- ein ganzer Spielfilm, 73 GB, beide Layer: die neu aufgebauten Streams sind **Byte für Byte
  identisch** mit der Disc, 59,33 GB Base Layer und 4,25 GB Enhancement Layer, auf beiden Seiten
  ohne Rest, und alle 166.928 ursprünglichen Metadatensätze der Disc wiederhergestellt. Mit Profil 7
  ebenso wie mit Profil 8.1, die Disc kommt also exakt zurück, während ihre Metadaten umgewandelt
  und wieder eingesetzt werden
- ein ganzer Titel mit **Seamless Branching**, 23 an 22 Stellen aneinandergefügte Clips: der
  Neuaufbau mit Profil 7 und der mit Profil 8.1 sind auf beiden Layern Byte für Byte identisch

Im Klartext: **Die Disc, die herauskommt, ist die Disc, die hineingegangen ist.**

### Wie die Startcode-Form erhalten bleibt

Eine Disc setzt ihre Startcodes auf die eine oder die andere Weise, mit drei Bytes oder mit vier.
Beides ist zulässig und wird identisch decodiert, doch eine einzelne Disc bleibt sich darin treu, und
Matroska speichert dieses Video mit Längenpräfix und enthält überhaupt keine Startcodes. Eine aus
Matroska neu aufgebaute Disc kam deshalb immer mit vier Bytes heraus, ganz gleich was die Quelle tat,
und eine Disc mit der kürzeren Form kam nie Byte für Byte zurück, so korrekt alles andere auch war.

Die Form der Quelle reist jetzt mit der Datei mit und wird wiederhergestellt. Sie wird pro NAL-Typ
festgehalten, denn genau so verfahren Discs, und eine pauschale Drei-Byte-Regel wäre nicht
normkonform. Ein NAL-Typ, der in einer Quelle in beiden Formen vorkommt, wird gar nicht festgehalten,
statt zu raten, und eine Quelle, die durchgehend vier Bytes verwendet, ebenso wenig, denn das
schreibt tsMuxeR ohnehin. Eine Datei ohne Besonderheiten trägt also nichts Zusätzliches, und der
schlimmste Fall ist das bisherige Verhalten und kein falsches. Nichts davon ist eine Besonderheit von
Dolby Vision: eine gewöhnliche Single-Layer-Disc, die nach Matroska und zurück geht, behält ihre Form
auf dem gleichen Weg.

### Der Rest der Disc

Die Aussage oben bezieht sich auf das Video. Audio, Untertitel und die Disc-Struktur werden neu
erstellt, wie bei jedem Muxen, und sie wurden gemessen und nicht angenommen. Dieselbe Disc auf zwei
Wegen erstellt, direkt aus ihren Layern und über eine Matroska-Datei:

| | Ergebnis |
| --- | --- |
| verlustfreies Audio | identisch, Byte für Byte |
| der AC-3-Kern daneben | ein Frame zu kurz, siehe unten |
| Untertitel | identisch, Byte für Byte |
| die Clip-Informationsdatei samt Suchindex | identisch, Byte für Byte |
| Playlist, Index und Movie Object | identisch, Byte für Byte |
| Program Map und Clock Reference | in jedem Feld identisch |
| der Aufbau des Transportstroms | identisch, abgesehen vom letzten Moment eines Schnitts |

**Der eine Schönheitsfehler:** Wird eine Disc aus einer Matroska-Datei erstellt, fehlt der letzte
Frame des AC-3-Kompatibilitätskerns, also 32 ms jenes Ersatzstroms, der für Player existiert, die den
verlustfreien Strom nicht decodieren können. Der verlustfreie Strom selbst ist vollständig und
identisch. Das ist nicht neu und keine Besonderheit von Dolby Vision: es passiert an der Stelle, an
der die beiden Audioströme wieder zu einem verflochten werden, weil dem letzten Kern-Frame kein
verlustfreier Frame mehr zur Seite steht.

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
