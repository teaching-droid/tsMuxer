[English](REPORTING_A_PROBLEM.md) | [Deutsch](REPORTING_A_PROBLEM_DE.md) | [Español](REPORTING_A_PROBLEM_ES.md) | [Français](REPORTING_A_PROBLEM_FR.md) | [Italiano](REPORTING_A_PROBLEM_IT.md) | [日本語](REPORTING_A_PROBLEM_JA.md) | [Português (Brasil)](REPORTING_A_PROBLEM_PT_BR.md) | [Português (Portugal)](REPORTING_A_PROBLEM_PT_PT.md) | [简体中文](REPORTING_A_PROBLEM_ZH.md)

# Relatar um problema

Quando um mux dá errado, dois arquivos do tsMuxeR ajudam a encontrar a falha sem enviar o filme inteiro:

| Arquivo | O que é | Tamanho | Quando enviar |
|---|---|---|---|
| **Relatório** | Um arquivo de texto da execução: o que o tsMuxeR recebeu, cada mensagem, o que cada faixa leu e deixou de fora, e o que foi gravado | Pequeno | Em qualquer problema |
| **Amostra** | Um arquivo .zip com uma parte da fonte, copiada byte a byte | Até 25 MB | Quando o relatório não basta, ou quando pedirem uma |

Os dois podem ser criados na janela ou na linha de comando. Anexe-os a uma nova issue com o [formulário "Bug report"](https://github.com/teaching-droid/tsMuxer/issues/new/choose). Você pode arrastar um arquivo para o formulário.

**Uma issue é pública.** Um relatório não contém imagem nem som, e substitui os nomes dos seus arquivos. Uma amostra contém uma parte do próprio filme, então anexe-a só se você puder compartilhá-la.

## Na janela

### Criar um relatório

1. Abra a aba **Geral** e marque **Gravar um relatório de cada mux para um relato de problema**.
2. Execute a tarefa de novo, para que o problema aconteça.
3. Na janela de progresso, clique em **Salvar relatório...** quando a tarefa terminar.
4. Leia o texto mostrado e depois salve-o.

### Criar uma amostra

1. Na aba **Entrada**, escolha o arquivo na lista e clique em **Criar uma amostra...**. A janela de progresso também oferece isso quando uma tarefa termina com erro.
2. Em **Pegar**, escolha **o início**, **o fim** ou **por volta deste tempo (onde está o problema)**, e informe o tempo.
3. Em **Tamanho**, mantenha o tamanho indicado ou escolha um menor. O arquivo .zip fica abaixo de 25 MB.
4. Marque **Adicionar o relatório da última tarefa**, se houver um. **Mostrar relatório** mostra-o antes.
5. Em **Salvar como**, escolha para onde vai o arquivo .zip e clique em **Criar amostra**.

## Na linha de comando

Adicione `--report` à tarefa que dá errado:

```
tsMuxeR --report=report.txt film.meta film.ts
```

Depois crie uma amostra por volta do tempo do problema, com esse relatório dentro:

```
tsMuxeR --make-sample --at=1:02:13 --add-report=report.txt film.mkv sample.zip
```

| Opção | O que faz |
|---|---|
| `--from-start` | Pega o início da fonte. É o padrão. |
| `--from-end` | Pega o fim da fonte. |
| `--at=<tempo>` | Pega a parte por volta de um tempo, como h:mm:ss ou em segundos. |
| `--size=<MB>` | Define o tamanho da amostra, de 1 a 24 MB. Sem indicação, é 20. |
| `--add-report=<arquivo>` | Coloca no .zip o relatório da tarefa que deu errado. |

O relatório não pode ter o nome de um arquivo da tarefa: uma fonte, o arquivo meta ou a saída. A amostra não pode ser gravada por cima da sua fonte nem do relatório que vai dentro dela. O tsMuxeR recusa esses nomes e avisa.

## O que o relatório contém

* a versão do tsMuxeR e o sistema: a versão dele, o número de processadores, a memória e a hora da execução
* a linha de comando e o arquivo meta
* o tamanho e os primeiros 32 bytes de cada fonte, e a listagem dela
* cada mensagem da execução, com o tempo em segundos, também as linhas que não aparecem na tela
* o que cada faixa leu, usou e deixou de fora
* os arquivos gravados, com seus tamanhos

**Seus nomes continuam privados.** As pastas são retiradas de cada caminho. Os nomes de arquivos, títulos, faixas e capítulos são substituídos por marcadores, como `<source1>.mkv`. Uma seção chamada Names traz só o necessário para achar uma falha ligada a um nome, como o comprimento dele e se tem espaços. Os nomes dos arquivos de um disco, como `00011.mpls`, são mantidos.

Só se pedirem: `--report-keep-names` mantém os nomes e `--report-full-paths` mantém também as pastas. `--report-frames` adiciona um segundo arquivo, bem maior, com uma linha para cada parte do fluxo gravada. Informe um intervalo de tempo, como `--report-frames=0:10:00-0:12:00`.

## O que a amostra contém

* **uma parte da fonte**, copiada byte a byte, para mostrar a mesma falha da fonte
* **um relatório da criação da amostra**, e o relatório da tarefa, se você o adicionou

No .zip, o arquivo se chama `source1`, seguido da sua própria extensão. Uma amostra Matroska mantém suas faixas e capítulos, mas o título e os nomes das faixas e capítulos ficam em branco. Tags, anexos e índice ficam de fora.

A amostra começa onde uma leitura pode começar, então pode iniciar um pouco antes do tempo informado. Uma fonte que cabe na amostra é pega inteira, e um tempo depois do fim pega o fim.

Algumas fontes têm limites:

| Fonte | Limite |
|---|---|
| MP4, MOV | Só o início pode ser pego: `--at` e `--from-end` são recusados. |
| Um fluxo de vídeo sozinho | Não contém tempos: pegue o início ou o fim dele. |
| Um arquivo dentro de uma imagem de disco | Informe o caminho depois da imagem, como `film.iso/BDMV/STREAM/00001.m2ts`. |

Para os detalhes de cada tipo de fonte, veja "Sample for a problem report" em [USAGE](USAGE.md) (em inglês).

## O que mais ajuda

* o que você esperava, e o que aconteceu no lugar disso
* para um problema de reprodução, o player ou aparelho que você usou

O relatório já contém a versão, o arquivo meta e todo o log: com um relatório, não é preciso colá-los.
