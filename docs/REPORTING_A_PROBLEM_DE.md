[English](REPORTING_A_PROBLEM.md) | [Deutsch](REPORTING_A_PROBLEM_DE.md) | [Español](REPORTING_A_PROBLEM_ES.md) | [Français](REPORTING_A_PROBLEM_FR.md) | [Italiano](REPORTING_A_PROBLEM_IT.md) | [日本語](REPORTING_A_PROBLEM_JA.md) | [Português (Brasil)](REPORTING_A_PROBLEM_PT_BR.md) | [简体中文](REPORTING_A_PROBLEM_ZH.md)

# Ein Problem melden

Wenn beim Muxen etwas schiefgeht, helfen zwei Dateien von tsMuxeR, den Fehler zu finden, ohne dass Sie den ganzen Film schicken müssen:

| Datei | Was sie ist | Größe | Wann Sie sie schicken |
|---|---|---|---|
| **Bericht** | Eine Textdatei zum Lauf: was tsMuxeR bekommen hat, jede Meldung, was jede Spur gelesen und ausgelassen hat und was geschrieben wurde | Klein | Bei jedem Problem |
| **Probe** | Eine .zip-Datei mit einem Teil der Quelle, Byte für Byte kopiert | Bis 25 MB | Wenn der Bericht nicht reicht oder wenn Sie darum gebeten werden |

Beides lässt sich im Fenster oder auf der Kommandozeile erstellen. Hängen Sie die Dateien an ein neues Issue mit dem [Formular "Bug report"](https://github.com/teaching-droid/tsMuxer/issues/new/choose) an. Sie können eine Datei einfach in das Formular ziehen.

**Ein Issue ist öffentlich.** Ein Bericht enthält weder Bild noch Ton und ersetzt Ihre Dateinamen. Eine Probe enthält einen Teil des Films selbst. Hängen Sie sie deshalb nur an, wenn Sie ihn weitergeben dürfen.

## Im Fenster

### Einen Bericht erstellen

1. Öffnen Sie den Reiter **Allgemein** und setzen Sie den Haken bei **Einen Bericht zu jedem Muxen für eine Problemmeldung schreiben**.
2. Starten Sie den Auftrag noch einmal, damit das Problem auftritt.
3. Klicken Sie im Fortschrittsfenster auf **Bericht speichern...**, wenn der Auftrag beendet ist.
4. Lesen Sie den angezeigten Text und speichern Sie ihn dann.

### Eine Probe erstellen

1. Wählen Sie auf dem Reiter **Eingabe** die Datei in der Liste und klicken Sie auf **Probe erstellen...**. Das Fortschrittsfenster bietet das auch an, wenn ein Auftrag mit einem Fehler endet.
2. Wählen Sie unter **Teil** entweder **den Anfang**, **das Ende** oder **um diese Zeit (wo das Problem auftritt)** und geben Sie die Zeit an.
3. Behalten Sie unter **Größe** den vorgegebenen Wert oder wählen Sie einen kleineren. Die .zip-Datei bleibt unter 25 MB.
4. Setzen Sie den Haken bei **Bericht des letzten Auftrags beifügen**, wenn es einen gibt. **Bericht anzeigen** zeigt ihn vorher an.
5. Wählen Sie unter **Speichern als**, wohin die .zip-Datei kommt, und klicken Sie auf **Probe erstellen**.

## Auf der Kommandozeile

Fügen Sie dem Auftrag, der schiefgeht, `--report` hinzu:

```
tsMuxeR --report=report.txt film.meta film.ts
```

Erstellen Sie dann eine Probe um die Zeit des Problems, mit diesem Bericht darin:

```
tsMuxeR --make-sample --at=1:02:13 --add-report=report.txt film.mkv sample.zip
```

| Option | Was sie tut |
|---|---|
| `--from-start` | Nimmt den Anfang der Quelle. Das ist die Vorgabe. |
| `--from-end` | Nimmt das Ende der Quelle. |
| `--at=<Zeit>` | Nimmt den Teil um eine Zeit, als h:mm:ss oder in Sekunden. |
| `--size=<MB>` | Legt die Größe der Probe fest, von 1 bis 24 MB. Ohne Angabe sind es 20. |
| `--add-report=<Datei>` | Legt den Bericht des fehlgeschlagenen Auftrags in die .zip-Datei. |

Der Bericht darf nicht wie eine Datei des Auftrags heißen: eine Quelle, die Metadatei oder die Ausgabe. Die Probe darf weder ihre Quelle noch den Bericht überschreiben, der in sie hineinkommt. tsMuxeR lehnt solche Namen ab und sagt das.

## Was der Bericht enthält

* die Version von tsMuxeR und das System: seine Version, die Anzahl der Prozessoren, den Arbeitsspeicher und die Zeit des Laufs
* die Kommandozeile und die Metadatei
* die Größe und die ersten 32 Bytes jeder Quelle sowie ihre Auflistung
* jede Meldung des Laufs mit ihrer Zeit in Sekunden, auch die Zeilen, die nicht auf dem Bildschirm erscheinen
* was jede Spur gelesen, verwendet und ausgelassen hat
* die geschriebenen Dateien mit ihrer Größe

**Ihre Namen bleiben privat.** Ordner werden aus jedem Pfad entfernt. Die Namen von Dateien, Titeln, Spuren und Kapiteln werden durch Platzhalter ersetzt, zum Beispiel `<source1>.mkv`. Ein Abschnitt namens Names enthält nur, was nötig ist, um einen Fehler mit einem Namen zu finden, etwa seine Länge und ob er Leerzeichen enthält. Die Namen der Dateien einer Disc, zum Beispiel `00800.mpls`, bleiben erhalten.

Nur wenn Sie darum gebeten werden: `--report-keep-names` behält die Namen und `--report-full-paths` zusätzlich die Ordner. `--report-frames` fügt eine zweite, viel größere Datei hinzu, mit einer Zeile für jeden geschriebenen Teil des Streams. Geben Sie dazu einen Zeitbereich an, zum Beispiel `--report-frames=0:10:00-0:12:00`.

## Was die Probe enthält

* **einen Teil der Quelle**, Byte für Byte kopiert, damit er denselben Fehler zeigt wie die Quelle
* **einen Bericht über das Erstellen der Probe** und den Bericht des Auftrags, wenn Sie ihn beigefügt haben

Die Datei heißt in der .zip-Datei `source1`, gefolgt von ihrer eigenen Endung. Eine Matroska-Probe behält ihre Spuren und Kapitel, aber ihr Titel und die Namen ihrer Spuren und Kapitel werden geleert. Tags, Anhänge und Index fehlen.

Die Probe beginnt an einer Stelle, an der ein Lesevorgang beginnen kann, also vielleicht etwas vor der angegebenen Zeit. Eine Quelle, die in die Probe passt, wird ganz genommen, und eine Zeit nach dem Ende nimmt das Ende.

Für einige Quellen gelten Grenzen:

| Quelle | Grenze |
|---|---|
| MP4, MOV | Nur der Anfang kann genommen werden, `--at` und `--from-end` werden abgelehnt. |
| Ein Videostream für sich | Er enthält keine Zeiten. Nehmen Sie seinen Anfang oder sein Ende. |
| Eine Datei in einem Disc-Image | Geben Sie ihren Pfad nach dem Image an, zum Beispiel `film.iso/BDMV/STREAM/00001.m2ts`. |

Einzelheiten zu jeder Art von Quelle stehen in [USAGE](USAGE.md) unter "Sample for a problem report" (auf Englisch).

## Was sonst noch hilft

* was Sie erwartet haben und was stattdessen passiert ist
* bei einem Wiedergabeproblem der Player oder das Gerät, das Sie verwendet haben

Der Bericht enthält bereits die Version, die Metadatei und das ganze Protokoll. Mit einem Bericht müssen Sie diese also nicht einfügen.
