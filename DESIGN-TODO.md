# Redesign do WORSHIP (em andamento)

Pedido: tela do teclado, cores dos pads, página do simulador e fluxo de uso. Clima: escuro e calmo (âmbar/dourado).

Já feito neste branch:
- `tools/gen_worship_fonts.sh`: fontes próprias do WORSHIP com acentos (WM 15 px, WL 26 px) e o tom gigante (XL 92 px);
  geradas por `tools/build.py` em `build/gen/worship_fonts.h` (o build do FM-1 não usa).
- `gfx.c`: HC_FONTS 6 com WORSHIP (as fontes novas são Huffman).

Falta (plano):
- Tela TOM: tom gigante (XL) centralizado com halo que respira com o pad; voz e transição; medidor fino;
  faixa TECLAS / LEAD / MODO; rodapé com aviso ou "PRÓXIMA 5 · G". Paleta própria quente.
- Tela SETLIST: 2 × 8 como os pads, atual em dourado; detalhes da música embaixo.
- Pads: âmbar calmo, o tom tocando respira; PARAR vermelho baixo; setlist dourado (não azul).
- Fluxo: tocar uma música no SETLIST volta sozinho para TOM; acentos nos textos (MÚSICA, TRANSIÇÃO, ÓRGÃO).
- Simulador: mesma paleta dourada.
