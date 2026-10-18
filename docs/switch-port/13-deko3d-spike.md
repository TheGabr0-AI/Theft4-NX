# 13. Prova di fattibilità deko3d (nessun porting)

Decisione dell'utente (2026-10-18): il renderer nativo sarà su deko3d, ma
prima una prova di fattibilità misurata. Qui: cosa c'è, cosa si è misurato sul
PC, cosa misura lo smoke sulla console (passo 8, nella stessa build degli altri
passi), e a quali condizioni numeriche deko3d conviene. Nessuna riga del
renderer è stata scritta.

**Stato delle misure:** D.1 e D.3 sono misurati (PC, 2026-10-18). D.2 è
**scritto, compilato, non ancora eseguito**: i numeri arrivano dal primo avvio
dello smoke (`out/build/switch-smoke/switch-smoke.nro`, righe `gpu bench:`).

## D.1 Disponibilità

| Cosa | Stato | Note |
|---|---|---|
| `libdeko3d` (devkitPro, ufficiale) | **installato**: pacchetto `deko3d 0.5.0-1`; `libnx/include/deko3d.h`, `deko3d.hpp`, `libnx/lib/libdeko3d.a` (e `libdeko3dd.a`, debug) | accanto a `libnx 4.12.0-1` |
| `uam` (devkitPro, ufficiale) | **installato**: pacchetto `uam 1.1.0-2`, `tools/bin/uam.exe` (build del 24-01-2022) | solo **GLSL** in ingresso (`-s vert/frag/…`, `-o .dksh`); niente SPIR-V |
| esempi | `examples/switch/graphics/deko3d/` (deko_basic, deko_console, deko_examples 01–09) | usati come riferimento per DKSH, descrittori, copie |
| link con NVK nello stesso eseguibile | **verificato**: lo smoke collega `libdeko3d.a` e `libvulkan.a` di mesa-switch insieme, senza simboli doppi | un'API alla volta a runtime (D.2) |

**Il fork `github.com/gnaghi/uam`** (letto il 2026-10-18): non è un fork di
`devkitPro/uam` ma di `averne/libuam`; 144 commit, 0 stelle, 0 fork. Accetta
SPIR-V (`-i spirv` o dal numero magico) e scrive un file di riflessione
(`-R`). Licenza: quella di uam (fincs, per i file non importati) più quella
di Mesa (MIT) per `mesa-imported/`: **compatibile**. Qualità per noi: il
README stesso dice che in SPIR-V **il campionamento delle texture è uno stub**
e che **SSBO e immagini non sono supportati**; geometry, tessellation e compute
"non provati". Nel nostro campione (D.3) **319 shader su 319 leggono lo
shared memory da uno SSBO** e 180 campionano texture: col fork non ne
passerebbe nessuno. **Non dipenderne.** La strada è SPIR-V → GLSL (SPIRV-Cross)
→ uam ufficiale, misurata in D.3.

## D.2 Micro-benchmark nello smoke (passo 8)

`tools/switch-smoke/gpu_bench.cpp`, cvar `nx_smoke_gpu_bench` (accesa), dopo
la sonda Vulkan del passo 7. Lo stesso carico, nello stesso processo, ai clock
del gioco (lo smoke chiede apm `0x92220008` con il codice di switch-game,
`nx_clocks.cpp`, e scrive i clock letti prima e dopo ogni API), fuori schermo
(niente swapchain). Un'API alla volta (il device dell'una è distrutto prima
che parta l'altra), due giri: NVK, deko3d, NVK, deko3d. Shader dallo stesso
GLSL (`tools/switch-smoke/gpu_bench_shaders/`, nostro, nessun contenuto del
gioco): glslang per NVK, uam per deko3d; `gen.py` rigenera
`gpu_bench_shaders.h`.

| Parte | Carico | Misura |
|---|---|---|
| 1. draw | 20 fotogrammi da **5.000 quad** piccoli su un target 1280×720 RGBA8; per draw un nuovo pezzo da 256 byte di uniform buffer e uno scissor, ogni 4 draw un'altra di 16 texture, ogni 8 un'altra di 4 pipeline (2 pixel shader × 2 blend) | CPU per draw per registrare, tempo di submit, **GPU ms per fotogramma** (timestamp), tempo a parete |
| 2. submit e fence | 200 submission vuote, ciascuna attesa | mediana e p95 in µs |
| 3. upload | immagine 2048×2048 RGBA8 (16 MiB) da un buffer host, 5 volte | memcpy a parte, copia a parete, GPU ms, MB/s |
| 4. fill-rate | 20 passate a schermo intero 1280×720 di uno shader pesante (32 giri di sin/cos) | GPU ms per passata, Mpixel/s |
| 5. render target | 400 cambi fra 4 target 1280×720, un quad ciascuno | CPU per cambio, GPU totale |

Equivalenze scelte (da tenere presenti leggendo i numeri): in NVK una
"pipeline" è un `VkPipeline` e il pezzo di uniform buffer un offset dinamico di
`vkCmdBindDescriptorSets` (16 set, uno per texture); in deko3d il cambio di
pipeline è `dkCmdBufBindShaders` + stato di colore + blend, il pezzo di
uniform buffer `dkCmdBufBindUniformBuffer`, la texture `dkCmdBufBindTexture`.
Cambio di render target: `vkCmdBeginRendering`/`vkCmdEndRendering` (dynamic
rendering) contro `dkCmdBufBindRenderTargets`. Timestamp convertiti con 625/384
ns per tick da entrambe le parti (`dkTimestampToNs`; per NVK vedi 06 §12). Le
mediane saltano il primo fotogramma o la prima ripetizione.

**Righe nel `smoke.log`:** `gpu bench: clocks …`, `gpu bench: NVK round N
draws: … us CPU per draw …`, `… submit+fence …`, `… upload 16 MiB …`, `… fill
…`, `… render targets …` (lo stesso per `deko3d`), due righe `gpu bench
SUMMARY (round 2, NVK / deko3d)`, poi `GPU BENCH OK` o `GPU BENCH FAIL`.
Errori di deko3d: righe `gpu bench: deko3d debug:` (callback di debug del
device).

### Risultati (da compilare col primo smoke)

| Misura | NVK | deko3d | Rapporto |
|---|---|---|---|
| CPU per draw (registrazione), µs | – | – | – |
| GPU per fotogramma da 5.000 draw, ms | – | – | – |
| submit + fence, µs (mediana / p95) | – | – | – |
| upload 16 MiB, GPU ms (MB/s) | – | – | – |
| fill 720p shader pesante, GPU ms per passata | – | – | – |
| cambio di render target, CPU µs / GPU ms per 400 | – | – | – |

## D.3 Shader: dal microcodice Xenos al DKSH

**Cosa producono oggi i traduttori:**

- **Traduttore di ReXGlue** (`SpirvShaderTranslator`, quello che gira sulla
  Switch): microcodice Xenos → **SPIR-V** a runtime, per ogni coppia (shader,
  modifica); per D3D12 lo stesso traduttore fa DXBC. Sulla Switch costa 8,4 ms
  per shader (run 30, riga `pipelines:`), poi NVK compila la pipeline
  (210–354 ms l'una in partita).
- **XenosRecomp** (sottomodulo `tools/XenosRecomp`, fork sonicnext-dev): Xenos →
  **HLSL**, compilato fuori linea (DXC: DXIL o SPIR-V; nel ramo iOS di questo
  repository AIR per Metal). Sulla Switch non è usato; richiede gli shader
  estratti prima dell'esecuzione.

**Campione:** tutti i **319 shader** (155 vertex, 164 pixel) della shader
storage della build PC dell'edizione europea (`545407F2.xsh`, microcodice
Xenos, uguale su ogni piattaforma; resta sul PC dell'utente, non nel
repository). Strumenti in `tools/shader-probe/`: `xsh_to_spirv` (il traduttore
dell'SDK compilato per il PC, `build.sh`), SPIRV-Cross (Khronos, commit
`300681b` del 2026-10-14, compilato sul PC), `vk_glsl_to_deko.py`, uam
ufficiale 1.1.0. Modifica di traduzione: quella di default con le uscite di
un draw comune (tutti gli interpolatori); feature del dispositivo: quelle di
NVK sulla Tegra X1 (11 §run 16), float controls spenti.

| Passo | Passano | Tempo (PC, i5-7300HQ 2,5 GHz) |
|---|---|---|
| microcodice → SPIR-V (`xsh_to_spirv`) | **319 / 319** | 2,2 ms per shader |
| SPIR-V → GLSL per OpenGL (SPIRV-Cross `--version 460 --no-es`) | **0 / 319** | – |
| SPIR-V → GLSL Vulkan (SPIRV-Cross `--vulkan-semantics`) | 319 / 319 | ~158 ms per shader (processo compreso) |
| GLSL Vulkan → GLSL per uam (`vk_glsl_to_deko.py`, 6 riscritture) | 319 / 319 | trascurabile |
| GLSL → DKSH (uam 1.1.0) | **319 / 319 compilano** | vertex 0,37 s di media (max 1,7 s), pixel 0,98 s (max 6,6 s); DKSH 13,5 KB / 32,0 KB di media |

**Compilano, non è detto che siano giusti:** nessuno è stato eseguito né
confrontato con l'uscita di NVK. È il passo seguente (un draw del gioco reso
con le due API e confrontato pixel per pixel).

**Costrutti che fanno fallire la strada diretta** (ciascuno è una cosa che il
traduttore dovrebbe emettere diversamente per deko3d):

| Costrutto | Shader | Perché fallisce | Aggiramento nella prova |
|---|---|---|---|
| blocco `XeSystemConstants` con layout `scalar` e tipi misti (`vec4` a offset 28) | 319 | in GLSL per OpenGL non esiste un layout che lo esprima; SPIRV-Cross non lo appiattisce (tipi diversi) | blocco std140 di `uvec4`, ogni membro letto da una funzione che ricompone le parole (`uintBitsToFloat`) |
| set di descrittori (`set = N`) | 319 | deko3d lega per stadio e per tipo, senza set | set tolti, binding rinumerati per tipo (≤ 16 uniform, ≤ 16 storage, ≤ 32 texture: limiti rispettati da tutti) |
| texture e sampler separati (`texture2DArray` + `sampler`) | 180 | uam vuole sampler combinati | una variabile `sampler2DArray` per ogni coppia usata |
| `spirv_instruction` NMin/NMax/NClamp (GLSL.std.450 79–81) | 311 | nessuna funzione GLSL equivalente (min/max che scelgono l'operando non NaN) | funzioni con `isnan` |
| `gl_VertexIndex` / `gl_InstanceIndex` | 155 | nomi Vulkan; in GLSL per OpenGL `gl_VertexID` | rinomina |
| `demote` (`GL_EXT_demote_to_helper_invocation`) | 19 | estensione non disponibile | `discard` (differenza nelle derivate dei pixel vicini: da verificare) |
| costanti di specializzazione | 0 nel campione | gli shader del gioco non ne hanno; le hanno gli shader dei trasferimenti EDRAM (`vulkan_transfer_pitch_specialization`) e FSI | servono varianti precompilate o costanti uniform |
| uscite e formati | nessun problema | 1 shader scrive `gl_FragDepth`; nessuna immagine di storage | – |

Tutti e 319 leggono lo shared memory (vertex fetch) da uno SSBO `std430`: uam
lo accetta.

**Precompilazione: al build no, all'avvio sulla console no, sul PC
dell'utente sì.** Al build no: gli shader sono del gioco (non vanno nel
repository) e le modifiche dipendono dallo stato dei draw a runtime. All'avvio
sulla console no: uam sul PC impiega in media 0,7 s per shader (~3,6 minuti
per i 319, che con le modifiche in partita diventano 353 nel run 30); sulla
Switch (A57 a 1020 MHz contro un i5 a 2,5–3,5 GHz) ci si aspetta un fattore
4–8, quindi 3–6 s per shader, cioè 20–35 minuti la prima volta e scatti di
secondi per ogni shader nuovo in partita (**da misurare**: uam su Switch non
esiste come libreria, andrebbe portato). Fattibile: un passo sul PC che legge
la shader storage del gioco (come questa prova), produce i DKSH per le
modifiche viste e li mette sulla SD; per gli shader nuovi in partita serve
comunque una via a runtime (NVK come ripiego, o uam portato). NVK oggi:
8,4 ms di traduzione + 210–354 ms di compilazione per pipeline, sulla
console.

## D.4 Raccomandazione

**Oggi: no, non ancora come scelta.** La conversione degli shader è fattibile
(319/319 compilano con sei riscritture meccaniche, da portare nel traduttore),
ma i numeri che decidono (D.2) non ci sono ancora, e ciò che limita il gioco
oggi non dipende dall'API (06 §30 C): il fotogramma è seriale fra command
processor e GPU (~440 ms di attesa + ~157 ms di CPU su ~640), due terzi del
tempo GPU sono l'emulazione dell'EDRAM (trasferimenti di ownership 49 %,
resolve e compute 15 %), il mixer audio è CPU del gioco. deko3d non toglie
nessuna di queste tre cose da solo: le toglie un renderer nativo che non
emula l'EDRAM, scritto su qualunque API.

**deko3d conviene come API del renderer nativo se il passo 8 dello smoke dà
almeno una delle due:**

1. **GPU:** fill-rate dello shader pesante e GPU per fotogramma da 5.000 draw
   **≥ 20 % migliori** di NVK (cioè il codice di uam/nv50_ir, Mesa 19, più
   veloce di quello di NAK sullo stesso GLSL). Al traguardo di 15 FPS
   (66,7 ms) il 20 % del tempo GPU vale ~13 ms per fotogramma.
2. **CPU:** risparmio per draw **≥ 1,6 µs** rispetto a NVK (NVK per draw meno
   deko3d per draw, registrazione + submit). Con ~4.100 draw per fotogramma vuol
   dire ≥ 6,6 ms, il 10 % del fotogramma a 15 FPS; sotto questa soglia la CPU
   non è un motivo per cambiare API.

**e a queste condizioni:** (a) i DKSH degli shader del gioco danno la stessa
immagine di NVK sul draw di prova (verifica pixel per pixel); (b) esiste una
via per gli shader che compaiono in partita (uam portato sulla console, con un
costo per shader misurato, o un ripiego); (c) il renderer nativo non emula
l'EDRAM (altrimenti il 64 % del tempo GPU resta). Se nessuna delle due soglie
è raggiunta, il renderer nativo va scritto su Vulkan (NVK), che ha già
pipeline cache su disco, timestamp e il traduttore che funziona, e i guadagni
vengono dal non emulare l'EDRAM e dal non serializzare CPU e GPU.
