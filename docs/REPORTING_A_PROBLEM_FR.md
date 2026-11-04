[English](REPORTING_A_PROBLEM.md) | [Deutsch](REPORTING_A_PROBLEM_DE.md) | [Español](REPORTING_A_PROBLEM_ES.md) | [Français](REPORTING_A_PROBLEM_FR.md) | [Italiano](REPORTING_A_PROBLEM_IT.md) | [日本語](REPORTING_A_PROBLEM_JA.md) | [Português (Brasil)](REPORTING_A_PROBLEM_PT_BR.md) | [简体中文](REPORTING_A_PROBLEM_ZH.md)

# Signaler un problème

Quand un multiplexage se passe mal, deux fichiers de tsMuxeR aident à trouver le défaut sans envoyer tout le film :

| Fichier | Ce que c'est | Taille | Quand l'envoyer |
|---|---|---|---|
| **Rapport** | Un fichier texte de l'exécution : ce que tsMuxeR a reçu, chaque message, ce que chaque piste a lu et laissé de côté, et ce qui a été écrit | Petite | Pour tout problème |
| **Échantillon** | Un fichier .zip avec une partie de la source, copiée octet par octet | Jusqu'à 25 Mo | Quand le rapport ne suffit pas, ou quand on vous en demande un |

Les deux se créent dans la fenêtre ou en ligne de commande. Joignez-les à une nouvelle issue avec le [formulaire "Bug report"](https://github.com/teaching-droid/tsMuxer/issues/new/choose). Vous pouvez faire glisser un fichier dans le formulaire.

**Une issue est publique.** Un rapport ne contient ni image ni son, et il remplace vos noms de fichiers. Un échantillon contient une partie du film lui-même : ne le joignez que si vous avez le droit de le partager.

## Dans la fenêtre

### Créer un rapport

1. Ouvrez l'onglet **Général** et cochez **Écrire un rapport de chaque multiplexage pour un signalement de problème**.
2. Relancez la tâche, pour que le problème se produise.
3. Dans la fenêtre de progression, cliquez sur **Enregistrer le rapport...** quand la tâche est terminée.
4. Lisez le texte affiché, puis enregistrez-le.

### Créer un échantillon

1. Dans l'onglet **Entrée**, choisissez le fichier dans la liste et cliquez sur **Créer un échantillon...**. La fenêtre de progression le propose aussi quand une tâche se termine par une erreur.
2. Sous **Partie**, choisissez **le début**, **la fin** ou **autour de ce moment (là où se trouve le problème)**, et indiquez le moment.
3. Sous **Taille**, gardez la taille proposée ou choisissez-en une plus petite. Le fichier .zip reste sous 25 Mo.
4. Cochez **Ajouter le rapport de la dernière tâche** s'il y en a un. **Afficher le rapport** le montre d'abord.
5. Sous **Enregistrer sous**, choisissez où va le fichier .zip, puis cliquez sur **Créer l'échantillon**.

## En ligne de commande

Ajoutez `--report` à la tâche qui se passe mal :

```
tsMuxeR --report=report.txt film.meta film.ts
```

Créez ensuite un échantillon autour du moment du problème, avec ce rapport à l'intérieur :

```
tsMuxeR --make-sample --at=1:02:13 --add-report=report.txt film.mkv sample.zip
```

| Option | Ce qu'elle fait |
|---|---|
| `--from-start` | Prend le début de la source. C'est la valeur par défaut. |
| `--from-end` | Prend la fin de la source. |
| `--at=<moment>` | Prend la partie autour d'un moment, en h:mm:ss ou en secondes. |
| `--size=<Mo>` | Fixe la taille de l'échantillon, de 1 à 24 Mo. Elle est de 20 si rien n'est indiqué. |
| `--add-report=<fichier>` | Met le rapport de la tâche qui a échoué dans le fichier .zip. |

Le rapport ne peut pas porter le nom d'un fichier de la tâche : une source, le fichier Meta ou la sortie. L'échantillon ne peut pas être écrit par-dessus sa source ni par-dessus le rapport qu'il contient. tsMuxeR refuse ces noms et le dit.

## Ce que contient le rapport

* la version de tsMuxeR et le système : sa version, le nombre de processeurs, la mémoire et l'heure de l'exécution
* la ligne de commande et le fichier Meta
* la taille et les 32 premiers octets de chaque source, ainsi que sa liste
* chaque message de l'exécution, avec son moment en secondes, y compris les lignes qui ne s'affichent pas à l'écran
* ce que chaque piste a lu, utilisé et laissé de côté
* les fichiers écrits, avec leur taille

**Vos noms restent privés.** Les dossiers sont retirés de chaque chemin. Les noms des fichiers, titres, pistes et chapitres sont remplacés par des marques, comme `<source1>.mkv`. Une section appelée Names ne donne que ce qui sert à trouver un défaut lié à un nom, comme sa longueur et s'il contient des espaces. Les noms des fichiers d'un disque, comme `00800.mpls`, sont conservés.

Seulement si on vous le demande : `--report-keep-names` conserve les noms et `--report-full-paths` conserve aussi les dossiers. `--report-frames` ajoute un second fichier, bien plus gros, avec une ligne pour chaque partie du flux écrite. Donnez-lui une plage de temps, comme `--report-frames=0:10:00-0:12:00`.

## Ce que contient l'échantillon

* **une partie de la source**, copiée octet par octet, pour qu'elle montre le même défaut que la source
* **un rapport de la création de l'échantillon**, et le rapport de la tâche si vous l'avez ajouté

Dans le fichier .zip, le fichier s'appelle `source1`, suivi de sa propre extension. Un échantillon Matroska garde ses pistes et ses chapitres, mais son titre et les noms de ses pistes et chapitres sont vidés. Ses tags, ses pièces jointes et son index sont laissés de côté.

L'échantillon commence là où une lecture peut commencer : il peut donc démarrer un peu avant le moment indiqué. Une source qui tient dans l'échantillon est prise en entier, et un moment après la fin prend la fin.

Certaines sources ont des limites :

| Source | Limite |
|---|---|
| MP4, MOV | Seul le début peut être pris : `--at` et `--from-end` sont refusés. |
| Un flux vidéo seul | Il ne contient pas de temps : prenez son début ou sa fin. |
| Un fichier dans une image disque | Donnez son chemin après l'image, comme `film.iso/BDMV/STREAM/00001.m2ts`. |

Pour le détail de chaque type de source, voir "Sample for a problem report" dans [USAGE](USAGE.md) (en anglais).

## Ce qui aide encore

* ce que vous attendiez, et ce qui s'est passé à la place
* pour un problème de lecture, le lecteur ou l'appareil utilisé

Le rapport contient déjà la version, le fichier Meta et tout le journal : avec un rapport, inutile de les coller.
