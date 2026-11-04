[English](REPORTING_A_PROBLEM.md) | [Deutsch](REPORTING_A_PROBLEM_DE.md) | [Español](REPORTING_A_PROBLEM_ES.md) | [Français](REPORTING_A_PROBLEM_FR.md) | [Italiano](REPORTING_A_PROBLEM_IT.md) | [日本語](REPORTING_A_PROBLEM_JA.md) | [Português (Brasil)](REPORTING_A_PROBLEM_PT_BR.md) | [简体中文](REPORTING_A_PROBLEM_ZH.md)

# Segnalare un problema

Quando un mux va storto, due file di tsMuxeR aiutano a trovare il difetto senza inviare tutto il film:

| File | Che cos'è | Dimensione | Quando inviarlo |
|---|---|---|---|
| **Report** | Un file di testo dell'esecuzione: che cosa ha ricevuto tsMuxeR, ogni messaggio, che cosa ogni traccia ha letto e tralasciato, e che cosa è stato scritto | Piccola | Per ogni problema |
| **Campione** | Un file .zip con una parte della sorgente, copiata byte per byte | Fino a 25 MB | Quando il report non basta, o quando viene richiesto |

Entrambi si creano nella finestra o dalla riga di comando. Allegateli a una nuova issue con il [modulo "Bug report"](https://github.com/teaching-droid/tsMuxer/issues/new/choose). Potete trascinare un file nel modulo.

**Una issue è pubblica.** Un report non contiene immagini né audio e sostituisce i nomi dei vostri file. Un campione contiene una parte del film stesso: allegatelo solo se potete condividerlo.

## Nella finestra

### Creare un report

1. Aprite la scheda **Generale** e spuntate **Scrivi un report di ogni mux per una segnalazione di problema**.
2. Eseguite di nuovo il lavoro, in modo che il problema si ripresenti.
3. Nella finestra di avanzamento, fate clic su **Salva report...** quando il lavoro è terminato.
4. Leggete il testo mostrato, poi salvatelo.

### Creare un campione

1. Nella scheda **Ingresso**, scegliete il file nell'elenco e fate clic su **Crea un campione...**. La finestra di avanzamento lo propone anche quando un lavoro termina con un errore.
2. In **Prendi**, scegliete **l'inizio**, **la fine** oppure **intorno a questo tempo (dove si trova il problema)**, e indicate il tempo.
3. In **Dimensione**, mantenete la dimensione proposta o sceglietene una minore. Il file .zip resta sotto i 25 MB.
4. Spuntate **Aggiungi il report dell'ultimo lavoro** se ce n'è uno. **Mostra report** lo mostra prima.
5. In **Salva come**, scegliete dove va il file .zip e fate clic su **Crea campione**.

## Dalla riga di comando

Aggiungete `--report` al lavoro che va storto:

```
tsMuxeR --report=report.txt film.meta film.ts
```

Poi create un campione intorno al tempo del problema, con quel report all'interno:

```
tsMuxeR --make-sample --at=1:02:13 --add-report=report.txt film.mkv sample.zip
```

| Opzione | Che cosa fa |
|---|---|
| `--from-start` | Prende l'inizio della sorgente. È il valore predefinito. |
| `--from-end` | Prende la fine della sorgente. |
| `--at=<tempo>` | Prende la parte intorno a un tempo, come h:mm:ss o in secondi. |
| `--size=<MB>` | Imposta la dimensione del campione, da 1 a 24 MB. Se non è indicata, è 20. |
| `--add-report=<file>` | Mette nel file .zip il report del lavoro andato storto. |

Il report non può avere il nome di un file del lavoro: una sorgente, il file meta o l'output. Il campione non può essere scritto sopra la sua sorgente né sopra il report che contiene. tsMuxeR rifiuta questi nomi e lo segnala.

## Che cosa contiene il report

* la versione di tsMuxeR e il sistema: la sua versione, il numero di processori, la memoria e l'ora dell'esecuzione
* la riga di comando e il file meta
* la dimensione e i primi 32 byte di ogni sorgente, e il suo elenco
* ogni messaggio dell'esecuzione, con il tempo in secondi, comprese le righe non mostrate sullo schermo
* che cosa ogni traccia ha letto, usato e tralasciato
* i file scritti, con la loro dimensione

**I vostri nomi restano privati.** Le cartelle vengono tolte da ogni percorso. I nomi di file, titoli, tracce e capitoli vengono sostituiti da segnaposto, come `<source1>.mkv`. Una sezione chiamata Names riporta solo ciò che serve per trovare un difetto legato a un nome, come la lunghezza e se contiene spazi. I nomi dei file di un disco, come `00800.mpls`, vengono mantenuti.

Solo se vi viene chiesto: `--report-keep-names` mantiene i nomi e `--report-full-paths` mantiene anche le cartelle. `--report-frames` aggiunge un secondo file, molto più grande, con una riga per ogni parte del flusso scritta. Indicate un intervallo di tempo, come `--report-frames=0:10:00-0:12:00`.

## Che cosa contiene il campione

* **una parte della sorgente**, copiata byte per byte, in modo che mostri lo stesso difetto della sorgente
* **un report della creazione del campione**, e il report del lavoro se l'avete aggiunto

Nel file .zip il file si chiama `source1`, seguito dalla sua estensione. Un campione Matroska conserva tracce e capitoli, ma il titolo e i nomi di tracce e capitoli vengono svuotati. Tag, allegati e indice vengono tralasciati.

Il campione inizia dove può iniziare una lettura, quindi può partire poco prima del tempo indicato. Una sorgente che entra nel campione viene presa per intero, e un tempo oltre la fine prende la fine.

Alcune sorgenti hanno dei limiti:

| Sorgente | Limite |
|---|---|
| MP4, MOV | Si può prendere solo l'inizio: `--at` e `--from-end` vengono rifiutati. |
| Un flusso video da solo | Non contiene tempi: prendete l'inizio o la fine. |
| Un file dentro un'immagine disco | Indicate il percorso dopo l'immagine, come `film.iso/BDMV/STREAM/00001.m2ts`. |

Per i dettagli di ogni tipo di sorgente, vedete "Sample for a problem report" in [USAGE](USAGE.md) (in inglese).

## Che cos'altro aiuta

* che cosa vi aspettavate, e che cosa è successo invece
* per un problema di riproduzione, il lettore o il dispositivo usato

Il report contiene già la versione, il file meta e tutto il log: con un report non serve incollarli.
