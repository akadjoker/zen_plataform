# Plano: substituir o SDL2 nos engines (ficheiros, paths, input, janela, OpenGL)

Objetivo: os engines `Radion`, `CocoShape` (usa o Radion), `Kinetix2D` e `iGUI` deixam
de depender do SDL2, passando a usar o `zen_platform`. Sem DLLs, build rápido e
binários pequenos. **Regra: só entra o que os engines usam.** Fora do plano:
threads, mutex, áudio, SDL_Renderer, logging, storage SDL3 completo.

Estado: Fase 1 em curso (Erros + paths).

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

### Fase 1 - Erros + paths
- `platform_get_error()` (thread-local).
- `path_join`, `path_normalize`, `path_is_absolute`, `path_relative`, `path_absolute`.
- Testes de tabela.

### Fase 2 - I/O sem `fopen`
- `IoStream` mínimo: abrir ficheiro (`r`, `w`, `a`), memória só de leitura, asset; `read/write/seek/tell/size/close`.
- `io_load_file` e `io_save_file`. A escrita é atómica: `.tmp`, depois `fsync`, depois `rename`.
- `file_read`/`file_write`/`asset_*` passam a assentar nesta camada.

### Fase 3 - Filesystem (o que o `std::filesystem` e o SDL dão aos engines)
- `fs_get_base_path`, `fs_get_pref_path(org, app)`, `fs_get_temp_path`, `fs_get_current_directory`.
- `fs_get_path_info` (tipo, tamanho, mtime), `fs_create_directory` (recursivo), `fs_remove_path`, `fs_rename_path`.
- `fs_enumerate_directory(path, recursive, cb, ud)`.
- Web: `pref_path` em IDBFS e `fs_sync()` para gravar de forma persistente.

### Fase 4 - OpenGL config
- `GLConfig` dentro de `WindowConfig`: profile (CORE/COMPAT/ES), versão, depth, stencil, msaa, debug, `share_with`.
- `gl_swap_interval` (incluindo -1 adaptive onde exista) e `gl_make_current(NULL)`.
- X11 (GLX_ARB_create_context, ES via GLX_EXT_create_context_es2_profile), Android (EGL), Web (WebGL), fake.

### Fase 5 - Input em falta
- Modificadores: `key_mods(w)` e `mods` no `EVENT_KEY`.
- Cursores NWSE/NESW/ALL. Cache dos cursores no X11: hoje cada mudança cria e liberta um cursor (`backend_x11.c:1170`).
- `mouse_capture(w, on)`, equivalente ao `SDL_CaptureMouse`.
- Texto: `text_input_start/stop` (teclado virtual no Android e na Web).
- `time_sleep(ms)`.

### Fase 6 - Gamepad
- API: `gamepad_count`, `gamepad_connected`, `gamepad_name`, `gamepad_button_down/pressed`, `gamepad_axis`, e eventos de ligar/desligar.
- Layout normalizado ao estilo `SDL_GameController`.
- Backends: Linux evdev (`/dev/input/event*`), Win32 XInput, Web Gamepad API, Android `AInputEvent`.

### Fase 7 - Backend Win32 nativo
- Janela, WGL com o GLConfig, input, I/O UTF-16, gamepad XInput.
- Runtime estático (`/MT`, `-static`). Teste: o `.exe` não importa DLLs fora do sistema.

### Fase 8 - Web e Android completos
- Web: IDBFS (`syncfs` no arranque, no `fs_sync` e no `pagehide`), `--preload-file`.
- Android: assets como stream e listagem de assets.

### Fase 9 - Integração nos engines
- `imgui_impl_zen` (backend de plataforma para o Dear ImGui) + o `imgui_impl_opengl3` existente.
- Guia de migração `SDL_X -> zen_y` com base na tabela da secção 1.
- Portar o Kinetix2D (o mais pequeno) primeiro, depois o Radion e o CocoShape.

## 4. Precisa de mais investigação
- `GLX_EXT_create_context_es2_profile`: suporte nos drivers alvo (o Kinetix2D pede ES 3.0 também no desktop?).
- Web: IDBFS clássico ou WasmFS/OPFS; depende da versão do emsdk usada.
- Android: `AAssetDir` não lista subdiretórios (opções: manifesto gerado no build, ou JNI).
- `ARCHITECTURE.md` e `CLAUDE.md` são citados no README mas não estão no repositório.
