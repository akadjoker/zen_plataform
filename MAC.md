# Para o Claude Code que está no Mac

Este ficheiro foi escrito pelo Claude que trabalha no PC Linux do dono (akadjoker), a pedido dele, porque não há forma de colar texto entre as duas máquinas. O dono confirma-o ao dizer-te para o leres. Estás no ramo `mac-real-1`, criado a partir de `mac-backend` (commit a935c43). **O backend do macOS está aqui, não no `main`:** `src/backend_cocoa.m`, `src/dialog_cocoa.m`, os testes `tests/*cocoa*`, `tools/mac_ci.sh`, `docs/MAC-PORT.md`.

## Regras do dono (o Mac é emprestado pela empresa)

- Trabalha só dentro da pasta onde este repositório está clonado. Não escrevas fora dela.
- Sem `sudo`, sem Homebrew e sem instalar nada global sem perguntares primeiro. Se faltar `cmake` ou `ninja`, propõe uma cópia numa pasta `tools/` ao lado do clone.
- Não alteres definições do sistema. Se um teste precisar de uma permissão do macOS (Acessibilidade, captura de ecrã), avisa antes: é o dono que aceita o diálogo.
- Os testes abrem janelas reais neste ecrã. Avisa antes de correres uma bateria que mexa no rato, no teclado ou na área de transferência.
- Git: trabalha neste ramo (`mac-real-1`). Nunca no `main`, nunca force-push. Commits pequenos, mensagens em português sem acentos no estilo do `git log`, sem linhas de co-autoria. Push no fim de cada passo: é assim que o Claude do Linux vê o teu trabalho e o revê.
- Não digas que um teste passou sem o teres corrido. O que não correres, diz que não correu.
- Corrige a biblioteca, não o teste, salvo quando o teste estiver errado e o provares.

## O que se sabe

- O backend nunca correu num Mac real: só num runner do GitHub (macOS 26, M1 virtual, um ecrã 1024x768 de escala 1). `docs/MAC-PORT.md` tem os factos de cada corrida.
- Última corrida (18 nov, commit 3fca0cd): compilou sem erros nas quatro configurações; testes nativos 31/35 em Release e 32/35 em ASan; backend fake 24/24.
- As quatro falhas têm correção escrita mas **não corrida** (commits 52506da, 1525b9e, 6568c9b, badd284): tipo MIME próprio na área de transferência, nome de ficheiro com acento num ficheiro largado, nível das janelas popup / tooltip, e o teste do rato com eventos postos na fila da aplicação.
- Este ramo está 75 commits atrás do `main` (seleção PRIMARY, Wayland, ficheiros largados com a posição do ponteiro).
- Vulkan: no runner o MoltenVK veio por Homebrew. Se não estiver neste Mac, essa parte fica de fora e dizes.

## O que fazer, por esta ordem

1. **Inventário, e parar.** Diz ao dono o que este Mac tem: versão do macOS, chip, `xcode-select -p`, `clang --version`, `cmake --version`, `ninja --version`, ecrãs e escala. Não compiles antes de ele ver.
2. **Compilar e testar** como o `tools/mac_ci.sh` faz (lê também `.github/workflows/mac.yml` e `tests/CMakeLists.txt`). Entrega a tabela teste a teste, ao lado da do `docs/MAC-PORT.md`.
3. **Confirmar ou desmentir as quatro correções.**
4. **O que o runner virtual não tinha:** ecrã Retina e mudança de escala (essas verificações foram saltadas), trackpad e gestos reais, um método de entrada real, GPU real.
5. **Só depois:** juntar o `main` a este ramo e adaptar o que o Cocoa precisar.
6. **No fim de cada passo:** acrescenta a `docs/MAC-PORT.md` uma secção com factos (máquina, o que correu, o que falhou e porquê), faz commit e push.

## Como falar com o Claude do Linux

Não há canal direto. Escreve o que precisares que ele veja no fim deste ficheiro, na secção abaixo, e faz push. O dono diz-lhe para fazer `git pull`, ele responde na mesma secção e faz push de volta.

## Mensagens

(vazio)
