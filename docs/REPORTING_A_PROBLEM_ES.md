[English](REPORTING_A_PROBLEM.md) | [Deutsch](REPORTING_A_PROBLEM_DE.md) | [Español](REPORTING_A_PROBLEM_ES.md) | [Français](REPORTING_A_PROBLEM_FR.md) | [Italiano](REPORTING_A_PROBLEM_IT.md) | [日本語](REPORTING_A_PROBLEM_JA.md) | [Português (Brasil)](REPORTING_A_PROBLEM_PT_BR.md) | [简体中文](REPORTING_A_PROBLEM_ZH.md)

# Informar de un problema

Cuando un multiplexado sale mal, dos archivos de tsMuxeR ayudan a encontrar el fallo sin enviar toda la película:

| Archivo | Qué es | Tamaño | Cuándo enviarlo |
|---|---|---|---|
| **Informe** | Un archivo de texto de la ejecución: lo que recibió tsMuxeR, cada mensaje, lo que cada pista leyó y dejó fuera, y lo que se escribió | Pequeño | Para cualquier problema |
| **Muestra** | Un archivo .zip con una parte de la fuente, copiada byte a byte | Hasta 25 MB | Cuando el informe no basta, o cuando se le pide una |

Ambos se crean en la ventana o en la línea de comandos. Adjúntelos a una nueva issue con el [formulario "Bug report"](https://github.com/teaching-droid/tsMuxer/issues/new/choose). Puede arrastrar un archivo al formulario.

**Una issue es pública.** Un informe no contiene imagen ni sonido, y sustituye los nombres de sus archivos. Una muestra contiene una parte de la propia película, así que adjúntela solo si puede compartirla.

## En la ventana

### Crear un informe

1. Abra la pestaña **General** y marque **Escribir un informe de cada multiplexado para un informe de problema**.
2. Ejecute la tarea de nuevo, para que se produzca el problema.
3. En la ventana de progreso, haga clic en **Guardar informe...** cuando la tarea haya terminado.
4. Lea el texto que se muestra y guárdelo.

### Crear una muestra

1. En la pestaña **Entrada**, elija el archivo en la lista y haga clic en **Crear una muestra...**. La ventana de progreso también lo ofrece cuando una tarea termina con un error.
2. En **Parte**, elija **el principio**, **el final** o **alrededor de este momento (donde está el problema)**, e indique el momento.
3. En **Tamaño**, mantenga el tamaño propuesto o elija uno menor. El archivo .zip se queda por debajo de 25 MB.
4. Marque **Añadir el informe de la última tarea** si hay uno. **Mostrar informe** lo muestra antes.
5. En **Guardar como**, elija dónde va el archivo .zip y haga clic en **Crear muestra**.

## En la línea de comandos

Añada `--report` a la tarea que sale mal:

```
tsMuxeR --report=report.txt film.meta film.ts
```

Después cree una muestra alrededor del momento del problema, con ese informe dentro:

```
tsMuxeR --make-sample --at=1:02:13 --add-report=report.txt film.mkv sample.zip
```

| Opción | Qué hace |
|---|---|
| `--from-start` | Toma el principio de la fuente. Es el valor por defecto. |
| `--from-end` | Toma el final de la fuente. |
| `--at=<momento>` | Toma la parte alrededor de un momento, como h:mm:ss o en segundos. |
| `--size=<MB>` | Fija el tamaño de la muestra, de 1 a 24 MB. Si no se indica, es 20. |
| `--add-report=<archivo>` | Pone en el .zip el informe de la tarea que salió mal. |

El informe no puede llamarse como un archivo de la tarea: una fuente, el archivo .meta o la salida. La muestra no puede escribirse encima de su fuente ni del informe que va dentro de ella. tsMuxeR rechaza esos nombres y lo dice.

## Qué contiene el informe

* la versión de tsMuxeR y el sistema: su versión, el número de procesadores, la memoria y la hora de la ejecución
* la línea de comandos y el archivo .meta
* el tamaño y los primeros 32 bytes de cada fuente, y su listado
* cada mensaje de la ejecución, con su momento en segundos, también las líneas que no se muestran en pantalla
* lo que cada pista leyó, usó y dejó fuera
* los archivos escritos, con su tamaño

**Sus nombres siguen siendo privados.** Las carpetas se quitan de cada ruta. Los nombres de archivos, títulos, pistas y capítulos se sustituyen por marcadores, como `<source1>.mkv`. Una sección llamada Names da solo lo necesario para encontrar un fallo con un nombre, como su longitud y si tiene espacios. Los nombres de los archivos de un disco, como `00800.mpls`, se conservan.

Solo si se le pide: `--report-keep-names` conserva los nombres y `--report-full-paths` conserva también las carpetas. `--report-frames` añade un segundo archivo, mucho mayor, con una línea por cada parte del flujo que se escribe. Indíquele un intervalo de tiempo, como `--report-frames=0:10:00-0:12:00`.

## Qué contiene la muestra

* **una parte de la fuente**, copiada byte a byte, para que muestre el mismo fallo que la fuente
* **un informe de la creación de la muestra**, y el informe de la tarea si lo añadió

En el .zip, el archivo se llama `source1`, seguido de su propia extensión. Una muestra Matroska conserva sus pistas y capítulos, pero su título y los nombres de sus pistas y capítulos se vacían. Sus etiquetas, adjuntos e índice se dejan fuera.

La muestra empieza donde puede empezar una lectura, así que puede comenzar un poco antes del momento indicado. Una fuente que cabe en la muestra se toma entera, y un momento posterior al final toma el final.

Algunas fuentes tienen límites:

| Fuente | Límite |
|---|---|
| MP4, MOV | Solo se puede tomar el principio: `--at` y `--from-end` se rechazan. |
| Un flujo de vídeo por sí solo | No contiene tiempos: tome su principio o su final. |
| Un archivo dentro de una imagen de disco | Indique su ruta después de la imagen, como `film.iso/BDMV/STREAM/00001.m2ts`. |

Para los detalles de cada tipo de fuente, vea "Sample for a problem report" en [USAGE](USAGE.md) (en inglés).

## Qué más ayuda

* lo que esperaba y lo que ocurrió en su lugar
* para un problema de reproducción, el reproductor o dispositivo que usó

El informe ya contiene la versión, el archivo .meta y todo el registro: con un informe no hace falta pegarlos.
