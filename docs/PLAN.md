# Plano: substituir o SDL2 nos engines (ficheiros, paths, input, janela, OpenGL)

Objetivo: os engines `Radion`, `CocoShape` (usa o Radion), `Kinetix2D` e `iGUI` deixam
de depender do SDL2, passando a usar o `zen_platform`. Sem DLLs, build rápido e
binários pequenos. **Regra: só entra o que os engines usam.** Fora do plano:
threads, mutex, áudio, SDL_Renderer, logging, storage SDL3 completo.

Estado: Fases 1 a 6 concluidas. Proxima: Fase 7.

## 1. Levantamento: o que os engines usam do SDL2

Contagem feita com `grep SDL_*` no código próprio, sem `vendor/`, `external/` nem `third_party/`.

| Área | Usado (Radion / Kinetix2D / iGUI) | zen hoje |
|---|---|---|
| Ficheiros | `SDL_RWFromFile` (`rb`/`wb`) + `RWsize/read/write/close` (leitura e escrita de ficheiros inteiros) | `file_read`/`file_write` com `fopen` |
| Paths | `SDL_GetBasePath`, `SDL_GetPrefPath`, e `std::filesystem`: path, last_write_time, exists, remove, is_directory, temp_directory_path, is_regular_file, current_path, relative, absolute, weakly_canonical, create_directories, rename, recursive_directory_iterator | parcial (`dir_*`, `path_*`) |
| Janela | create (centered/display, resizable, hidden, maximized), fullscreen desktop, title/size/pos, min/max/restore, flags, raise/focus, displays, bounds, usable bounds, eventos de resize/close/min/max/restore | ✅ quase tudo |
| Teclado | scancodes (conjunto completo, incluindo keypad), KEYDOWN/KEYUP, TEXTINPUT, `SDL_StartTextInput`, `SDL_GetModState` | teclas ✅, texto ✅ (codepoint), **modificadores ❌** |
| Rato | botões, movimento, roda (incluindo flipped), relative mode, warp, `SDL_CaptureMouse`, cursores de sistema | ✅ menos capture; **faltam cursores NWSE/NESW/ALL** |
| Toque | FINGERDOWN/MOTION/UP | ✅ |
| Gamepad | `SDL_GameControllerOpen/GetButton/GetAxis/Name`, `SDL_NumJoysticks`, `SDL_IsGameController` (Radion) | **❌ não existe** |
| OpenGL | `SDL_GL_SetAttribute` (major/minor, profile CORE ou **ES**, depth, stencil, doublebuffer, **DEBUG flag**, share context no imgui), `SetSwapInterval`, `MakeCurrent`, `GetProcAddress`, `GetDrawableSize` | versão + msaa + vsync; perfil core fixo; sem debug/ES/share |
| Tempo | `GetPerformanceCounter/Frequency`, `GetTicks`, `SDL_Delay` | `time_nanos` ✅, **sleep ❌** |
| Clipboard | get/set/has | ✅ |
| Dear ImGui | `imgui_impl_sdl2` no Radion e no Kinetix2D (`AddEventWatch`, `ProcessEvent`, `NewFrame`) | **❌ falta um backend `imgui_impl_zen`** |

Plataformas referenciadas no código: `_WIN32` (muito), `__EMSCRIPTEN__` (Kinetix2D), `__ANDROID__` (Kinetix2D).

Não entra no zen (fica nos engines):
- `SDL_Thread`/`SDL_mutex`/`SDL_cond` no Radion: passam para `std::thread`/`std::mutex`/`std::condition_variable`, trabalho a fazer no Radion.
- `SDL_Renderer` no adapter sdl2 do iGUI: o iGUI precisa de um adapter zen (draw2d ou GL), trabalho a fazer no iGUI.

## 2. Princípios
- Um contrato público (`platform.h`) e uma implementação por plataforma. Tudo estático, sem dependências de terceiros.
- No núcleo não se usa `fopen` nem stdio: POSIX `open/read/write/lseek/fsync`, Win32 `CreateFileW`, Android `AAsset_*`.
- Sem alocações escondidas: as listagens usam callback; os buffers alocados libertam-se com `fs_free`.
- Caminhos em UTF-8 e com `/` na API. No Win32, conversão interna para UTF-16.
- Cada fase fecha com testes verdes (também com ASan e UBSan), sem warnings, e com um exemplo a correr na plataforma alvo.

## 3. Fases

### Fase 1 - Erros + paths (concluida)
- `platform_get_error()` (thread-local).
- `path_join`, `path_normalize`, `path_is_absolute`, `path_relative`, `path_absolute`.
- Testes de tabela.

### Fase 2 - I/O sem `fopen` (concluida; `io_win32.c` fica para a Fase 7)
- `IoStream` mínimo: abrir ficheiro (`r`, `w`, `a`), memória só de leitura, asset; `read/write/seek/tell/size/close`.
- `io_load_file` e `io_save_file`. A escrita é atómica: `.tmp`, depois `fsync`, depois `rename`.
- `file_read`/`file_write`/`asset_*` passam a assentar nesta camada.

### Fase 3 - Filesystem (concluida em POSIX; Win32 na Fase 7, IDBFS na Fase 8)
- `fs_get_base_path`, `fs_get_pref_path(org, app)`, `fs_get_temp_path`. `dir_current` já cobre o diretório atual.
- `fs_get_path_info` (tipo, tamanho, mtime), `fs_create_directory` (recursivo), `fs_remove_path`, `fs_rename_path`.
- `fs_enumerate_directory(path, recursive, cb, ud)`.
- Web: o `pref_path` usa `$HOME/.local/share`; a persistência (IDBFS e `fs_sync`) fica na Fase 8.

### Fase 4 - OpenGL config (concluida em X11; Android e Web escritos mas nao compilados; WGL na Fase 7)
- `GLConfig` dentro de `WindowConfig`: profile (CORE/COMPAT/ES), versao, msaa e debug. Zero e o valor por omissao.
- Depth 24, stencil 8 e double buffer ficam fixos: os engines pedem sempre estes valores.
- Um pedido que a plataforma nao satisfaz faz o `window_create` falhar com mensagem em `platform_get_error()`, sem recuar de versao em silencio.
- `share_with` e `gl_make_current(NULL)` ficam de fora: nenhum engine os usa (o `share` so aparece no multi-viewport do `imgui_impl_sdl2`).
- O swap interval continua em `window_set_vsync`.
- X11: `GLX_ARB_create_context`, `GLX_EXT_create_context_es2_profile` e handler de erros do X, para uma versao impossivel devolver NULL em vez de terminar o processo.

### Fase 5 - Input em falta (concluida em X11; Web e Android escritos mas nao compilados)
- Teclas que o Radion mapeia e o zen nao tinha: teclado numerico (`KEY_KP_0`..`KEY_KP_9`, decimal, as quatro operacoes, enter, igual), `KEY_MENU` e `KEY_SCROLL_LOCK`. Em X11 traduzem-se tambem os keysyms de navegacao (`KP_Home`, `KP_End`...) que o servidor entrega com o NumLock desligado. A Web e o Android passam a mapear tambem NumLock, PrintScreen e Pause (e, no Android, Insert, CapsLock e Meta).
- `key_mods(w)`: mascara `KEYMOD_*` das teclas premidas agora, o equivalente ao `SDL_GetModState`. O campo `mods` dos eventos de tecla ja existia.
- O core liberta todas as teclas e botoes do rato quando a janela perde o foco, para um modificador nao ficar preso depois de um Alt-Tab.
- Cursores `CURSOR_RESIZE_NWSE`, `CURSOR_RESIZE_NESW` e `CURSOR_RESIZE_ALL`. No X11 os cursores passam a ser criados uma vez e partilhados por todas as janelas, em vez de criados e libertados a cada mudanca.
- `time_sleep(milliseconds)`.
- Fora do plano, porque nenhum engine usa: cursor com imagem propria, `CURSOR_WAIT`, `text_input_start/stop` (so aparece em exemplos do iGUI), `SDL_HasClipboardText` (equivale a `clipboard_get()[0] != 0`) e a direcao da roda (`MOUSEWHEEL_FLIPPED`).
- `SDL_CaptureMouse`: no X11 o servidor ja entrega os eventos fora da janela enquanto um botao esta premido. O backend Win32 deve fazer `SetCapture` ao premir e `ReleaseCapture` ao largar, sem API publica.

### Fase 6 - Gamepad (concluida em Linux; XInput na Fase 7)
- API por polling, como o teclado: `gamepad_connected`, `gamepad_name`, `gamepad_button_down`, `gamepad_axis`. Ate `GAMEPAD_MAX` (4) comandos, com a ordem de botoes e eixos do `SDL_GameController`. Sticks de -1 a 1 com Y para baixo, gatilhos de 0 a 1, sem deadzone. As arestas (premido e largado neste frame) ficam a cargo do engine, como o Radion ja faz.
- Linux (`gamepad_evdev.c`): le `/dev/input/event*`, deteta hot-plug com inotify (IN_CREATE, IN_ATTRIB e IN_DELETE), trata `SYN_DROPPED` e a desligacao do dispositivo. Reconhece o layout padrao do kernel (BTN_SOUTH/EAST/NORTH/WEST, ABS_X/Y/RX/RY, ABS_Z/RZ ou BTN_TL2/TR2 para os gatilhos, HAT0 ou BTN_DPAD para a cruz). Nao ha base de dados de mapeamentos: um comando que nao siga o layout do kernel nao aparece.
- Fora do plano, porque nenhum engine usa: gamepad na Web e no Android, rumble, giroscopio, touchpad, LEDs, eventos de ligar e desligar, joysticks sem layout padrao.
- Nao testado: o `ioctl` de sondagem e de ressincronizacao num dispositivo real. O container nao tem `uinput` nem `/dev/input`, por isso a deteccao, a leitura (via pipe), o hot-plug (inotify sobre um diretorio temporario) e a traducao dos eventos foram testados, mas a sondagem de um comando fisico nao.

### Fase 7 - Backend Win32 nativo
- Janela, WGL com o GLConfig, input, I/O UTF-16, gamepad XInput.
- Runtime estático (`/MT`, `-static`). Teste: o `.exe` não importa DLLs fora do sistema.

### Fase 8 - Web e Android completos
- Web: IDBFS (`syncfs` no arranque, no `fs_sync` e no `pagehide`), `--preload-file`.
- Android: assets como stream e listagem de assets.

### Fase 9 - Integração nos engines
- `imgui_impl_zen` (backend de plataforma para o Dear ImGui) + o `imgui_impl_opengl3` existente.
- Radion: o seu `GamepadButton` segue a ordem do raylib (LEFT_FACE_UP, ...) mas `Input::update` usa esse indice diretamente como `SDL_GameControllerButton`, que tem outra ordem. Ao migrar e preciso converter entre as duas; confirmar se o comportamento atual ja esta trocado.
- Guia de migração `SDL_X -> zen_y` com base na tabela da secção 1.
- Portar o Kinetix2D (o mais pequeno) primeiro, depois o Radion e o CocoShape.

## 4. Precisa de mais investigação
- `GLX_EXT_create_context_es2_profile`: suporte nos drivers alvo (o Kinetix2D pede ES 3.0 também no desktop?).
- Web: IDBFS clássico ou WasmFS/OPFS; depende da versão do emsdk usada.
- Android: `AAssetDir` não lista subdiretórios (opções: manifesto gerado no build, ou JNI).
- `ARCHITECTURE.md` e `CLAUDE.md` são citados no README mas não estão no repositório.
