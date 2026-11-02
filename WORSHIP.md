# WORSHIP para o M-VAVE SMK-37 Pro

Firmware customizado para tocar no culto: **pad ambiente por tom**, **setlist** com 16 músicas e
**teclas + lead** (para solos ou somado). Feito em cima do [Felucca](https://github.com/hugelton/Felucca)
com a camada SMK-37 do [genkigenki](https://github.com/genkigenki/fm1-to-smk37). O teclado liga direto
no WORSHIP; o botão **GLOBE** abre o sintetizador completo do Felucca e volta.

> **Simulador:** https://jefersomusic-del.github.io/worship-smk37/ (o firmware rodando no navegador).

> **Estado:** compilado e testado em simulação (testes do pad, teste de ponta a ponta do firmware SMK-37
> no computador e o simulador no navegador). **Ainda não rodou num SMK-37 de verdade.** Faça o backup antes
> e teste com calma antes de levar para um culto.

## Como funciona

| Controle | No WORSHIP |
|---|---|
| **Pads (vista TOM)** | Os 12 tons como um piano: em cima C D E F G A B, embaixo C# D# F# G# A#. Um toque liga o pad naquele tom; trocar de tom faz um crossfade (não corta). O último pad de cima (vermelho) **para** o pad; tocar de novo o tom que está soando também para. |
| **Pads (vista SETLIST)** | As 16 músicas. **Toque** carrega a música (tom, BPM, timbres, modo, pad). **Segure 1 segundo** para salvar o que está tocando agora naquela música. |
| **SEQ** | Alterna TOM e SETLIST |
| **ARP** | MODO das teclas: **TECLAS** (só o timbre de teclas), **SOLO** (só o lead), **SOMA** (os dois juntos: o acorde nas teclas, a melodia no lead) e **DIVIDIR** (lead do C5 para cima) |
| **CHORD** | Liga / desliga o pad (liga no tom da música) |
| **NOTE REPEAT** | TAP tempo (o LED pisca no tempo) |
| **&lt; &gt;** | Música anterior / próxima do setlist (o pad acompanha o tom da música) |
| **OCT− / OCT+** | Oitava do teclado |
| **BT** | SALVAR na música atual |
| **PATCH** | Silencia teclas e lead (o pad continua) |
| **GLOBE** | Vai para o sintetizador completo (Felucca) e volta |
| **K1** | Volume geral |
| **K2 / K3 / K4** | Timbre das TECLAS / do LEAD / do PAD |
| **K5 / K6** | BRILHO do pad / TRANSIÇÃO (0,3 a 8 s de crossfade) |
| **K7 / K8** | DELAY do lead (colcheia pontuada no BPM) / BPM |
| **Faders 1–4** | TECLAS, PAD, LEAD, AMBIÊNCIA (reverb dos três) |
| **Pitch** | Curva o lead (em TECLAS, o timbre de teclas) |
| **Mod** | Swell do pad: abre o filtro e sobe um pouco o volume |

**Timbres de teclas:** Piano, E.Piano, Órgão, Cordas, Coral, Synth Pad, Harpa, Sino, Marimba, Kalimba, Órgão FM, Sine Keys.
**Leads:** Saw Lead, Flauta, Sopro, Sync Lead, Wave Lead, Sax, Voz, Brass, Arp Lead, Pluck.
**Pads:** Quente, Etéreo, Escuro, Shimmer, Cordas. O pad não tem terça (tônica, quinta, oitavas), então
serve para música maior e menor.

O pad tem gerador próprio: não ocupa nenhuma das 8 vozes do teclado, então o piano nunca perde notas por
causa dele.

## Preparando o culto

1. Na vista TOM, escolha o tom no pad e os timbres (K2–K4), o BPM (K8 ou TAP) e o modo (ARP).
2. Aperte **SEQ** e **segure** o pad da posição da música (1 a 16) por 1 segundo: "SALVO NA MUSICA n".
3. Repita para cada música. No culto, use **&lt; &gt;** para avançar; o pad troca de tom sozinho.

> Salvar grava na memória flash e o áudio para por alguns milissegundos. Monte o setlist **antes** do culto.
> O firmware nunca grava sozinho enquanto o pad está soando.

## Instalar (Mac)

Leia antes o [SMK37.md](SMK37.md) e o guia do kit
[fm1-to-smk37](https://github.com/genkigenki/fm1-to-smk37#readme). Firmware customizado é por sua conta e risco.

1. **Firmware oficial V016** no teclado, com o atualizador da M-VAVE. Guarde o arquivo V016 do seu modelo:
   ele é o caminho de volta.
2. **Backup completo da flash.** Ela guarda a calibração das teclas, que é única de cada teclado. Siga o passo
   "Before you install anything, make a full flash backup" do kit fm1-to-smk37.
3. **Instalar:** com o teclado no USB, no Terminal, dentro desta pasta:
   ```sh
   pip3 install mido python-rtmidi
   python3 tools/fm1_install.py worship-smk37-pro-XXXXXXX.fwsc
   ```
   Não desconecte o cabo até o teclado reiniciar.
4. **Voltar ao original:** o atualizador da M-VAVE com o arquivo V016 do seu modelo.

## Compilar

O pacote `.fwsc` sai do GitHub Actions (`.github/workflows/worship.yml`): envie este repositório para o
GitHub, abra **Actions > worship-smk37**, e baixe o artefato `worship-smk37` (o `.fwsc`, as telas e um
áudio de exemplo do pad). Uma tag `v0.1` cria uma release.

No Mac, local: Docker Desktop e o [BUILDING.md](BUILDING.md), depois
`AC79_SDK=$PWD/vendor/ac79 BOARD=smk37 ./build.sh` → `build/felucca-smk37.fwsc`.

Testes no computador (sem o teclado):

```sh
cc -O2 -Ibuild/gen -Ifirmware/src -o build/worship_test tests/worship_test.c -lm && build/worship_test pad.wav
cc -O1 -w -ffp-contract=off -std=gnu99 -Ibuild/gen -Ifirmware/src -Ifirmware/hal -o build/worship_host \
   tests/worship_host_test.c -lm && build/worship_host build/worship_out
web/smk/build.sh      # o simulador: build/smk/worship-sim.html
```

## O que mudou no código

| Arquivo | O quê |
|---|---|
| `firmware/src/worship_dsp.c` | O pad: até 3 acordes soando (crossfade), 5 notas × 2 osciladores, saw com polyBLEP, filtro, swell |
| `firmware/src/worship.c` | Modo WORSHIP: setlist e flash (0xB1000 / 0xB2000, A/B com CRC), timbres, rota das teclas, botões, knobs, faders, LEDs dos pads, tela |
| `firmware/hal/smk_input.h` | Pads em modo RAW (eventos para o WORSHIP em vez de notas) |
| `seq.c`, `fx.c`, `ui_input.c`, `ui_draw.c`, `main.c`, `project.c`, `felucca.c` | Ganchos curtos, todos atrás de `FELUCCA_WORSHIP` (ligado só no SMK-37). O build do FM-1 não muda |
| `web/emu/smk_web.c`, `web/smk/` | O firmware SMK-37 inteiro no computador / navegador |
| `tests/worship_test.c`, `tests/worship_host_test.c` | Testes |
| `vendor/ac79/` | Os 3 arquivos do SDK da JieLi que o pacote precisa (Apache-2.0) |

A gravação na flash só usa a área livre do Felucca (`0xB1000–0xB2FFF`, dentro de FL_DATA); a calibração,
o Bluetooth e o key_mac continuam protegidos pela lista de faixas permitidas do driver.

GPL-3.0-only, como o Felucca. Créditos: Felucca (Leo Kuroshita, Hügelton Instruments), porte SMK-37
(genkigenki), WORSHIP (Jeferson Mesquita, IPB Mata da Praia).
