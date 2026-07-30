# EVER Enhanced v2.0.0-beta.2

[English](#english) · [Português (Brasil)](#português-brasil) · [Español](#español)

---

## English

### What this release is

EVER Enhanced is the GTA V Enhanced port of Extended Video Export Revived. It
replaces Rockstar Editor's limited capture and encoding path while preserving
the normal editor workflow. The output uses the real game/ReShade render target,
the selected frame rate, codec, audio settings, and motion-blur configuration.

This is a **public beta**. The runtime hooks are specific to validated game
builds; check the compatibility notice in the repository before installing it
after a GTA update.

### Validated environment

| Component | Validated version |
| --- | --- |
| GTA V Enhanced | `1.0.1158.13` |
| Steam public build | `24129078` |
| Rockstar title update | `1.73` |
| ScriptHookV | `3889.0.1158.13` |
| ReShade | `6.7.3`, DirectX 12, full add-on support |
| Validation date | July 30, 2026 |

The mod is storefront-independent in principle, but the exact executable and
end-to-end validation record above came from the Steam build.

### New in beta.2

- Complete release documentation in English, Brazilian Portuguese, and Spanish.
- A clearer installation-first repository layout and correct download guidance
  for prerelease assets.
- Localized documentation and these release notes included inside the ZIP.
- A clean tagged rebuild whose runtime log identifies `v2.0.0-beta.2`.

The validated runtime feature set is unchanged from beta.1; beta.2 improves the
public package, version identity, and user-facing release documentation.

### What changed from Legacy EVER

- **DirectX 12 capture:** Enhanced no longer uses the Legacy D3D11 texture and
  swap-chain capture path. ReShade add-on events track the full-resolution
  `VideoEncode` render target, resource transitions, and the clean pre-UI image.
- **Clean high-resolution output:** the capture source is selected before
  Rockstar's export progress interface and watermark. EVER refuses to silently
  upscale a smaller Media Foundation frame to the requested resolution.
- **Asynchronous GPU readback:** a three-slot D3D12 ring and fences allow GTA,
  the GPU, and the encoder to work in parallel without a synchronous screenshot
  stall on every frame.
- **Enhanced-specific game hooks:** custom FPS, export start, replay cleanup,
  pending-bake, and kill/cancel transitions use new signatures and complete
  entry validation. Unknown builds fail closed instead of applying Legacy
  addresses.
- **Rebuilt synchronized-audio handoff:** above 60 effective FPS, the first pass
  records PCM audio and the second renders video at the configured timing. The
  Enhanced Rockstar Editor state machine is handled on the correct main-thread
  polling and finalization paths.
- **D3D12 GPU motion blur:** temporal samples are accumulated in an RGBA16F GPU
  target and only the divided output frame is read back. CPU accumulation remains
  available when required, including the synchronized OpenEXR depth path.
- **Enhanced OpenEXR:** full-resolution color plus D32/S8 plane-0 depth readback,
  correct D3D12 row-pitch handling, and background EXR compression.
- **Self-contained FFmpeg runtime:** Voukoder is not required. Codec/container
  presets and the runtime DLLs used by this build ship inside the release ZIP.

### Validated export matrix

| Export | Result |
| --- | --- |
| 3840×2160, 30 FPS, H.264 NVENC, AAC | Passed; 463 frames, 1.33 ms A/V delta |
| 3840×2160, 60 FPS, H.264 NVENC, AAC | Passed; 926 frames, zero dropped frames |
| 3840×2160, 24 FPS + OpenEXR color/depth | Passed; 371 video frames and 371 EXRs |
| 3840×2160, 120 FPS, automatic dual pass | Passed; 1,852 frames, synchronized AAC |
| 3840×2160, 240 FPS, automatic dual pass | Passed; 3,704 frames at exact 240/1 FPS |
| 4K 30 FPS + 1 blur sample | Passed; 60 effective renders/s |
| 4K 30 FPS + 3 blur samples | Passed; 120 effective renders/s and synchronized audio |
| AV1 NVENC, 4K 120 FPS + 3 blur samples | Passed; 480 effective renders/s, zero drops |
| H.264 NVENC, H.265 NVENC, AV1 NVENC | Passed at 4K |
| MP4/AAC, MKV/FLAC, AVI/MP3, MOV/PCM | Passed end to end |

The effective render rate is:

```text
output FPS × (motion_blur_samples + 1)
```

An effective rate above 60 activates the automatic audio-first/video-second
workflow. Do not close the game between the two passes or while the final file
is still being written.

### Included capabilities

- Custom integer frame rates validated from 24 through 240 FPS.
- Resolution based on the active game/ReShade render size, including tested 4K.
- H.264, H.265/HEVC, AV1, VP9, ProRes, and OpenEXR presets.
- NVIDIA NVENC, AMD AMF, Intel QSV, and CPU encoder options where supported.
- MP4/AAC, MKV/FLAC, AVI/MP3, MOV/PCM, and WebM/Opus configurations.
- ReShade post-processing in the captured output.
- Clean output without Rockstar's export interface.
- Synchronized OpenEXR color and depth sequences.

Hardware encoder availability depends on the installed GPU and driver. The
included AV1 NVENC preset uses 8-bit YUV420 and four encoder surfaces to leave
VRAM available for GTA, ReShade, and graphics mods.

### Requirements

- A legally installed copy of GTA V Enhanced for Windows.
- A [ScriptHookV](https://www.dev-c.com/gtav/scripthookv/) version that
  explicitly supports the installed Enhanced build.
- [ReShade](https://reshade.me/) installed for `GTA5_Enhanced.exe`, with
  DirectX 12 and **full add-on support**.
- Sufficient disk space and VRAM for the chosen resolution, codec, and sample
  count.

ScriptHookV and ReShade are not included in this archive.

### Installation

1. Under **Assets**, download `EVER-Enhanced-v2.0.0-beta.2.zip`. Do not download
   GitHub's automatically generated “Source code” archives; they do not contain
   the compiled mod or its runtime DLLs.
2. Download [ScriptHookV from its official page](https://www.dev-c.com/gtav/scripthookv/)
   and install a compatible version beside `GTA5_Enhanced.exe`.
3. Download [ReShade from its official website](https://reshade.me/) and install
   it for `GTA5_Enhanced.exe`; select DirectX 12 and the build with full add-on
   support.
4. Close the game and back up any previous `zEVER.asi` and `EVER` directory.
5. Extract the release ZIP directly into the directory containing
   `GTA5_Enhanced.exe`.

Expected layout:

```text
GTA V Enhanced/
|-- GTA5_Enhanced.exe
|-- zEVER.asi
`-- EVER/
    |-- EVER.ini
    |-- preset.json
    |-- EncoderConfig.exe
    |-- dlls/
    `-- presets/
```

Do not rename `zEVER.asi` and do not put it inside the `EVER` directory. The `z`
prefix ensures ReShade is loaded before the EVER add-on.

### First launch and first export

1. Start Story Mode.
2. Press `Home`, open ReShade's add-ons tab, and confirm `GTA V Enhanced` and
   `Enhanced compatibility mode`.
3. For the first test, use a short Rockstar Editor project at 1920×1080, 30 FPS,
   H.264, and `motion_blur_samples = 0`.
4. After the baseline succeeds, increase resolution/FPS and then add motion blur
   one setting at a time.
5. With a blank `output_folder`, the result is written to the current Windows
   user's Videos directory. The exact path is also recorded in `EVER/EVER.log`.

### Updating from an earlier EVER build

Close GTA V, back up your current `EVER.ini` and custom presets, remove the old
`zEVER.asi` plus old `EVER` runtime files, and extract the complete new ZIP. Do
not mix DLLs from different releases. Reapply your settings after comparing the
new default configuration.

Legacy GTA V users must use a compatible release from the upstream EVER project;
this package is for GTA V Enhanced.

### Known limitations

- Fractional frame rates still require explicit in-game validation.
- FiveM has not been validated by this Enhanced test suite.
- Native G-buffer planes beyond color, SubsurfaceScatter compatibility, and
  depth have not been identified.
- ProRes and CPU AV1 are demanding at 4K.
- Future GTA updates can change internal functions and require a new EVER build.

### Troubleshooting essentials

- If EVER does not appear, verify the Enhanced executable, DirectX 12, full
  ReShade add-on support, ScriptHookV compatibility, and the folder layout.
- For `ERR_GFX_STATE`, close the game fully, disable unrelated overlays/mods,
  and test the 1080p/30/no-blur baseline.
- If the encoder cannot reserve memory, reduce resolution, FPS, motion-blur
  samples, encoder surfaces, or other GPU-heavy applications.
- If an export appears frozen, check `EVER/EVER.log`; dual-pass handoff and
  asynchronous finalization can keep the export screen visible temporarily.
- When reporting a bug, attach the game/ReShade/ScriptHookV versions, GPU and
  driver, active configuration, and a debug log. Never upload game files or
  credentials.

### Integrity and uninstall

Compare the ZIP's SHA-256 with `EVER-Enhanced-v2.0.0-beta.2.zip.sha256`. To
uninstall, close the game and remove only `zEVER.asi` and the `EVER` directory
installed by this release. ReShade and ScriptHookV are separate products.

---

## Português (Brasil)

### O que é esta versão

O EVER Enhanced é a adaptação do Extended Video Export Revived para GTA V
Enhanced. Ele substitui a captura e a codificação limitadas do Editor Rockstar,
mas mantém o fluxo normal do editor. O resultado usa a resolução real do
jogo/ReShade, o FPS, codec, áudio e motion blur escolhidos pelo usuário.

Esta é uma **beta pública**. Os hooks dependem da versão validada do jogo;
confira o aviso de compatibilidade no repositório depois de qualquer atualização
do GTA.

### Ambiente validado

| Componente | Versão validada |
| --- | --- |
| GTA V Enhanced | `1.0.1158.13` |
| Build pública da Steam | `24129078` |
| Atualização de título Rockstar | `1.73` |
| ScriptHookV | `3889.0.1158.13` |
| ReShade | `6.7.3`, DirectX 12, suporte completo a add-ons |
| Data da validação | 30 de julho de 2026 |

### Novidades da beta.2

- Documentação completa da versão em inglês, português brasileiro e espanhol.
- Instalação colocada em destaque e orientação correta para baixar os Assets de
  uma pré-versão.
- READMEs traduzidos e estas notas incluídos dentro do ZIP.
- Nova compilação limpa cuja versão interna é `v2.0.0-beta.2`.

O conjunto de recursos validado é o mesmo da beta.1; a beta.2 melhora o pacote
público, a identificação da versão e sua documentação.

### O que mudou em relação ao EVER Legacy

- A captura D3D11 foi substituída por eventos do ReShade e recursos DirectX 12.
- O mod encontra o render target limpo e em resolução completa antes da UI de
  exportação e da marca-d'água da Rockstar.
- Um anel assíncrono de três readbacks evita bloquear CPU e GPU a cada quadro.
- Foram criadas assinaturas e validações específicas para as funções internas do
  Editor Rockstar do Enhanced.
- O fluxo de áudio em duas passagens foi refeito para a nova máquina de estados:
  primeiro PCM, depois vídeo no FPS e motion blur escolhidos.
- O motion blur acumula as amostras em `RGBA16F` na GPU DirectX 12 e lê apenas o
  quadro final dividido.
- OpenEXR recebeu captura sincronizada de cor/profundidade e compressão em
  segundo plano.
- O pacote usa diretamente FFmpeg e não exige Voukoder.

### Testes concluídos

- 4K em 24, 30, 60, 120 e 240 FPS.
- H.264 NVENC, H.265 NVENC e AV1 NVENC em 4K.
- AV1 NVENC em 4K 120 FPS com três amostras adicionais de motion blur: 480
  renderizações efetivas por segundo, zero quadros perdidos e áudio sincronizado.
- OpenEXR 4K com 371 quadros de cor e profundidade.
- MP4/AAC, MKV/FLAC, AVI/MP3 e MOV/PCM em exportações completas.
- Captura sem a interface, progresso ou marca-d'água da exportação Rockstar.

A taxa efetiva é `FPS × (motion_blur_samples + 1)`. Acima de 60, o EVER executa
automaticamente uma passagem de áudio e outra de vídeo. Não feche o jogo entre
as passagens nem durante a finalização do arquivo.

### Requisitos

- GTA V Enhanced original para Windows, e não a versão Legacy.
- [ScriptHookV](https://www.dev-c.com/gtav/scripthookv/) que declare suporte à
  build instalada do Enhanced.
- [ReShade](https://reshade.me/) no `GTA5_Enhanced.exe`, com DirectX 12 e
  **suporte completo a add-ons**.
- Espaço em disco e VRAM suficientes para a configuração escolhida.

ScriptHookV e ReShade não estão incluídos no ZIP.

### Instalação

1. Em **Assets**, baixe `EVER-Enhanced-v2.0.0-beta.2.zip`. Não use os arquivos
   “Source code” automáticos do GitHub: eles não contêm o mod compilado.
2. Baixe o [ScriptHookV na página oficial](https://www.dev-c.com/gtav/scripthookv/)
   e instale uma versão compatível ao lado de `GTA5_Enhanced.exe`.
3. Baixe o [ReShade no site oficial](https://reshade.me/) e instale no
   `GTA5_Enhanced.exe`, escolhendo DirectX 12 e suporte completo a add-ons.
4. Feche o jogo e faça backup de qualquer `zEVER.asi` e pasta `EVER` anteriores.
5. Extraia todo o ZIP diretamente na pasta que contém `GTA5_Enhanced.exe`.

```text
GTA V Enhanced/
|-- GTA5_Enhanced.exe
|-- zEVER.asi
`-- EVER/
    |-- EVER.ini
    |-- preset.json
    |-- EncoderConfig.exe
    |-- dlls/
    `-- presets/
```

Não renomeie `zEVER.asi` nem o mova para dentro da pasta `EVER`. O prefixo `z`
garante que o ReShade carregue antes do add-on.

### Primeiro teste

1. Abra o Modo História.
2. Pressione `Home`, abra a aba de add-ons do ReShade e confirme `GTA V Enhanced`
   e `Enhanced compatibility mode`.
3. Comece com um projeto curto em 1920×1080, 30 FPS, H.264 e
   `motion_blur_samples = 0`.
4. Depois, aumente resolução/FPS e adicione motion blur uma configuração por vez.
5. Com `output_folder` vazio, o vídeo fica na pasta Vídeos do usuário do Windows;
   o caminho exato também aparece em `EVER/EVER.log`.

### Atualização, limitações e desinstalação

Ao atualizar, feche o GTA, salve seu `EVER.ini` e presets personalizados e não
misture DLLs de versões diferentes. FPS fracionário e FiveM ainda não foram
validados, e futuras atualizações do GTA podem exigir novos hooks. ProRes e AV1
por CPU são pesados em 4K.

Confira a integridade pelo arquivo `.zip.sha256`. Para desinstalar, feche o jogo
e remova somente `zEVER.asi` e a pasta `EVER` instalados por esta versão.

---

## Español

### Qué es esta versión

EVER Enhanced es la adaptación de Extended Video Export Revived para GTA V
Enhanced. Sustituye la captura y codificación limitadas del Editor Rockstar,
conservando su flujo normal. El resultado usa la resolución real del
juego/ReShade y los ajustes elegidos de FPS, códec, audio y desenfoque.

Esta es una **beta pública**. Los hooks dependen de la versión validada del
juego; revisa el aviso de compatibilidad después de cualquier actualización.

### Entorno validado

| Componente | Versión validada |
| --- | --- |
| GTA V Enhanced | `1.0.1158.13` |
| Compilación pública de Steam | `24129078` |
| Actualización de título Rockstar | `1.73` |
| ScriptHookV | `3889.0.1158.13` |
| ReShade | `6.7.3`, DirectX 12, compatibilidad completa de complementos |
| Fecha de validación | 30 de julio de 2026 |

### Novedades de beta.2

- Documentación completa en inglés, portugués brasileño y español.
- Instalación destacada y orientación correcta para descargar los Assets de una
  versión preliminar.
- Documentación traducida y estas notas incluidas dentro del ZIP.
- Nueva compilación limpia cuya versión interna es `v2.0.0-beta.2`.

El conjunto de funciones validado es el mismo de beta.1; beta.2 mejora el
paquete público, la identificación de versión y la documentación.

### Qué cambió frente a EVER Legacy

- La captura D3D11 fue sustituida por eventos de ReShade y recursos DirectX 12.
- El mod localiza la imagen limpia a resolución completa antes de la interfaz y
  la marca de agua de exportación de Rockstar.
- Un anillo asíncrono de tres lecturas evita bloquear CPU y GPU en cada imagen.
- Se añadieron firmas y validaciones específicas para las funciones internas del
  Editor Rockstar de Enhanced.
- El flujo de audio en dos pasadas fue adaptado a la nueva máquina de estados:
  primero PCM y después vídeo con los FPS y desenfoque configurados.
- El desenfoque acumula muestras en un objetivo `RGBA16F` de la GPU DirectX 12 y
  solo lee el fotograma final dividido.
- OpenEXR admite color/profundidad sincronizados y compresión en segundo plano.
- El paquete usa FFmpeg directamente y no necesita Voukoder.

### Pruebas completadas

- 4K a 24, 30, 60, 120 y 240 FPS.
- H.264 NVENC, H.265 NVENC y AV1 NVENC en 4K.
- AV1 NVENC 4K 120 FPS con tres muestras adicionales de desenfoque: 480
  renderizados efectivos por segundo, cero pérdidas y audio sincronizado.
- OpenEXR 4K con 371 imágenes de color y profundidad.
- MP4/AAC, MKV/FLAC, AVI/MP3 y MOV/PCM en exportaciones completas.
- Captura sin interfaz, progreso ni marca de agua de exportación de Rockstar.

La tasa efectiva es `FPS × (motion_blur_samples + 1)`. Por encima de 60, EVER
ejecuta automáticamente una pasada de audio y otra de vídeo. No cierres el juego
entre ambas ni durante la escritura final.

### Requisitos

- GTA V Enhanced legal para Windows, no la versión Legacy.
- [ScriptHookV](https://www.dev-c.com/gtav/scripthookv/) que admita expresamente
  la compilación instalada de Enhanced.
- [ReShade](https://reshade.me/) para `GTA5_Enhanced.exe`, con DirectX 12 y
  **compatibilidad completa de complementos**.
- Espacio en disco y VRAM suficientes para la configuración elegida.

ScriptHookV y ReShade no están incluidos en el ZIP.

### Instalación

1. En **Assets**, descarga `EVER-Enhanced-v2.0.0-beta.2.zip`. No uses los archivos
   “Source code” automáticos de GitHub: no contienen el mod compilado.
2. Descarga [ScriptHookV desde su página oficial](https://www.dev-c.com/gtav/scripthookv/)
   e instala una versión compatible junto a `GTA5_Enhanced.exe`.
3. Descarga [ReShade desde su sitio oficial](https://reshade.me/) e instálalo para
   `GTA5_Enhanced.exe`, seleccionando DirectX 12 y compatibilidad completa de
   complementos.
4. Cierra el juego y guarda una copia de cualquier `zEVER.asi` y carpeta `EVER`
   anteriores.
5. Extrae todo el ZIP en la carpeta que contiene `GTA5_Enhanced.exe`.

```text
GTA V Enhanced/
|-- GTA5_Enhanced.exe
|-- zEVER.asi
`-- EVER/
    |-- EVER.ini
    |-- preset.json
    |-- EncoderConfig.exe
    |-- dlls/
    `-- presets/
```

No cambies el nombre de `zEVER.asi` ni lo muevas dentro de `EVER`. El prefijo
`z` hace que ReShade se cargue antes del complemento.

### Primera prueba

1. Abre el Modo Historia.
2. Pulsa `Home`, abre la pestaña de complementos de ReShade y comprueba
   `GTA V Enhanced` y `Enhanced compatibility mode`.
3. Empieza con un proyecto corto a 1920×1080, 30 FPS, H.264 y
   `motion_blur_samples = 0`.
4. Después aumenta resolución/FPS y añade desenfoque un ajuste cada vez.
5. Con `output_folder` vacío, el vídeo se guarda en Vídeos del usuario de
   Windows; la ruta exacta también aparece en `EVER/EVER.log`.

### Actualización, limitaciones y desinstalación

Para actualizar, cierra GTA, guarda tu `EVER.ini` y perfiles personalizados y no
mezcles DLL de versiones distintas. Las tasas fraccionarias y FiveM aún no han
sido validados, y futuras actualizaciones de GTA pueden exigir hooks nuevos.
ProRes y AV1 por CPU son exigentes en 4K.

Comprueba la integridad con el archivo `.zip.sha256`. Para desinstalar, cierra el
juego y elimina únicamente `zEVER.asi` y la carpeta `EVER` instalados por esta
versión.

---

## License and credits / Licença e créditos / Licencia y créditos

EVER Enhanced is distributed under the [Apache License 2.0](https://github.com/rossignolli/EVER-Enhanced/blob/main/LICENSE). The
release archive includes project notices and licenses for bundled third-party
libraries. Original EVE: Ali Alidoust. EVER revival: MiffedLyric, TheGreyRaven,
and contributors. GTA V Enhanced port: EVER Enhanced contributors.

Grand Theft Auto, GTA V, and Rockstar Games are trademarks of their respective
owners. This community project is not affiliated with or endorsed by Rockstar
Games.
