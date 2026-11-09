[English](REPORTING_A_PROBLEM.md) | [Deutsch](REPORTING_A_PROBLEM_DE.md) | [Español](REPORTING_A_PROBLEM_ES.md) | [Français](REPORTING_A_PROBLEM_FR.md) | [Italiano](REPORTING_A_PROBLEM_IT.md) | [日本語](REPORTING_A_PROBLEM_JA.md) | [Português (Brasil)](REPORTING_A_PROBLEM_PT_BR.md) | [Português (Portugal)](REPORTING_A_PROBLEM_PT_PT.md) | [简体中文](REPORTING_A_PROBLEM_ZH.md)

# Comunicar um problema

Quando um mux corre mal, dois ficheiros do tsMuxeR ajudam a encontrar a falha sem enviar o filme inteiro:

| Ficheiro | O que é | Tamanho | Quando enviar |
|---|---|---|---|
| **Relatório** | Um ficheiro de texto da execução: o que o tsMuxeR recebeu, cada mensagem, o que cada faixa leu e deixou de fora, e o que foi gravado | Pequeno | Em qualquer problema |
| **Amostra** | Um ficheiro .zip com uma parte da origem, copiada byte a byte | Até 25 MB | Quando o relatório não basta, ou quando for pedida uma |

Os dois podem ser criados na janela ou na linha de comandos. Anexe-os a uma nova issue com o [formulário «Bug report»](https://github.com/teaching-droid/tsMuxer/issues/new/choose). É possível arrastar um ficheiro para o formulário.

**Uma issue é pública.** Um relatório não contém imagem nem som, e substitui os nomes dos seus ficheiros. Uma amostra contém uma parte do próprio filme, por isso anexe-a só se a puder partilhar.

## Na janela

### Criar um relatório

1. Abra o separador **Geral** e marque **Gravar um relatório de cada mux para um relato de problema**.
2. Execute a tarefa de novo, para que o problema aconteça.
3. Na janela de progresso, clique em **Guardar relatório...** quando a tarefa terminar.
4. Leia o texto mostrado e depois guarde-o.

### Criar uma amostra

1. No separador **Entrada**, escolha o ficheiro na lista e clique em **Criar uma amostra...**. A janela de progresso também oferece isto quando uma tarefa termina com erro.
2. Em **Extrair**, escolha **o início**, **o fim** ou **por volta deste tempo (onde está o problema)**, e indique o tempo.
3. Em **Tamanho**, mantenha o tamanho indicado ou escolha um menor. O ficheiro .zip fica abaixo de 25 MB.
4. Marque **Adicionar o relatório da última tarefa**, se houver um. **Mostrar relatório** mostra-o antes.
5. Em **Guardar como**, escolha para onde vai o ficheiro .zip e clique em **Criar amostra**.

## Na linha de comandos

Adicione `--report` à tarefa que corre mal:

```
tsMuxeR --report=report.txt film.meta film.ts
```

Depois crie uma amostra por volta do tempo do problema, com esse relatório lá dentro:

```
tsMuxeR --make-sample --at=1:02:13 --add-report=report.txt film.mkv sample.zip
```

| Opção | O que faz |
|---|---|
| `--from-start` | Extrai o início da origem. É a predefinição. |
| `--from-end` | Extrai o fim da origem. |
| `--at=<tempo>` | Extrai a parte à volta de um tempo, como h:mm:ss ou em segundos. |
| `--size=<MB>` | Define o tamanho da amostra, de 1 a 24 MB. Se não for indicado, é 20. |
| `--add-report=<ficheiro>` | Coloca no .zip o relatório da tarefa que correu mal. |

O relatório não pode ter o nome de um ficheiro da tarefa: uma origem, o ficheiro meta ou a saída. A amostra não pode ser gravada por cima da sua origem nem do relatório que vai dentro dela. O tsMuxeR recusa esses nomes e avisa.

## O que o relatório contém

* a versão do tsMuxeR e o sistema: a versão deste, o número de processadores, a memória e a hora da execução
* a linha de comandos e o ficheiro meta
* o tamanho e os primeiros 32 bytes de cada origem, e a sua listagem
* cada mensagem da execução, com o tempo em segundos, incluindo as linhas que não aparecem no ecrã
* o que cada faixa leu, usou e deixou de fora
* os ficheiros gravados, com os seus tamanhos

**Os seus nomes continuam privados.** As pastas são retiradas de cada caminho. Os nomes de ficheiros, títulos, faixas e capítulos são substituídos por marcadores, como `<source1>.mkv`. Uma secção chamada Names traz só o necessário para encontrar uma falha ligada a um nome, como o seu comprimento e se tem espaços. Os nomes dos ficheiros de um disco, como `00011.mpls`, são mantidos.

Só se for pedido: `--report-keep-names` mantém os nomes e `--report-full-paths` mantém também as pastas. `--report-frames` adiciona um segundo ficheiro, muito maior, com uma linha para cada parte do fluxo que é gravada. Indique um intervalo de tempo, como `--report-frames=0:10:00-0:12:00`.

## O que a amostra contém

* **uma parte da origem**, copiada byte a byte, para mostrar a mesma falha que a origem
* **um relatório da criação da amostra**, e o relatório da tarefa, se o tiver adicionado

No .zip, o ficheiro chama-se `source1`, seguido da sua própria extensão. Uma amostra Matroska mantém as suas faixas e capítulos, mas o título e os nomes das faixas e capítulos ficam em branco. As tags, os anexos e o índice ficam de fora.

A amostra começa onde uma leitura pode começar, por isso pode iniciar um pouco antes do tempo indicado. Uma origem que cabe na amostra é extraída inteira, e um tempo depois do fim extrai o fim.

Algumas origens têm limites:

| Origem | Limite |
|---|---|
| MP4, MOV | Só o início pode ser extraído, por isso `--at` e `--from-end` são recusados. |
| Um fluxo de vídeo sozinho | Não contém tempos, por isso extraia o seu início ou o seu fim. |
| Um ficheiro dentro de uma imagem de disco | Indique-o pelo caminho depois da imagem, como `film.iso/BDMV/STREAM/00001.m2ts`. |

Para os pormenores de cada tipo de origem, veja «Sample for a problem report» em [USAGE](USAGE.md) (em inglês).

## O que mais ajuda

* o que esperava, e o que aconteceu em vez disso
* para um problema de reprodução, o leitor ou aparelho que usou

O relatório já contém a versão, o ficheiro meta e todo o registo: com um relatório, não é preciso colá-los.
