# Documentacao operacional para IA - TBH Companion Agent

Este arquivo e a porta de entrada para agentes de IA trabalharem no agente C++ do TaskBarHero Companion. Ele segue o padrao de documentacao operacional para IA em formato condensado: mapa de leitura, specs, skills, harnesses, seguranca, governanca e metricas em um unico documento versionavel.

O codigo continua sendo a fonte final. Antes de editar qualquer rotina, abra os arquivos reais citados aqui e confirme o comportamento atual.

## Ordem de leitura

1. Leia `README.md` para contexto de build, release, mapa IL2CPP e export de assets.
2. Leia este arquivo ate a secao do dominio afetado.
3. Abra `src/main.cpp` ou o script relevante antes de editar.
4. Use a skill operacional correspondente.
5. Rode o harness indicado.
6. Se uma regra importante for descoberta, atualize esta documentacao.

## Resumo executivo

O repo `tbh-companion-agent` contem um worker Win32 C++ puro que roda no PC do jogador. Ele:

- salva configuracao local em `%LOCALAPPDATA%\TBH Companion\config.ini`;
- abre/minimiza na bandeja;
- inicia o jogo pelo Steam se necessario;
- le o save ES3 oficial do TaskBarHero;
- extrai resumo de save com gold, stage atual, herois, itens equipados, inventario/storage, runas, pets e monster kills;
- le eventos vivos diretamente da memoria do jogo via `LogManager`/IL2CPP;
- classifica eventos de clear, falha, morte, baus e drops;
- envia payload incremental para o frontend remoto em `POST /api/ingest`;
- embute `res/items.zip` no executavel para resolver dados de item;
- possui scripts para recalcular offsets IL2CPP por versao do jogo;
- possui scripts para exportar assets Unity e gerar `frontend-pack` para o frontend.

O agente C++ e a fonte runtime atual. O Python antigo nao deve ser usado como implementacao de producao; scripts Python permanecem apenas como ferramentas de extracao, dump, comparacao e atualizacao de mapa.

## Mapa rapido de arquivos

| Area | Arquivos principais | Observacoes |
| --- | --- | --- |
| Agente Win32 | `src/main.cpp` | UI nativa, tray, config, leitura ES3, leitura de memoria, payload, HTTP sync. |
| Build | `build.bat`, `src/app.rc`, `src/resource.h`, `res/app.ico` | Compila com MSVC Build Tools e resource compiler. |
| Items embutidos | `res/items.json`, `res/items.zip`, `src/generated_items.h`, `scripts/build_items.ps1` | `build.bat` gera zip e SHA1 antes do `cl`. |
| IL2CPP map | `scripts/refresh_il2cpp_map.py`, bloco `IL2CPP MAP` em `src/main.cpp` | Recalcula offsets por versao do jogo e patcha frontend. |
| Release | `scripts/release.ps1` | Recalcula mapa, compila, commita, push e publica GitHub Release. |
| Assets Unity | `scripts/export_unity_assets.ps1`, `scripts/extract_unity_localization.py`, `exported-assets/` | Organiza AssetRipper export em `frontend-pack`. |
| Frontend pack | `exported-assets/TaskBarHero-<versao>/frontend-pack/` | Fonte para rebuild do frontend. |
| Referencia de batalha | `scripts/extract_battle_reference.py`, `docs/battle-reference-<versao>.*` | Cruza AssetRipper, CSVs, prefabs, AnimationClips e dump IL2CPP para features visuais de combate. |
| Analise DPS/memoria | `docs/dps-memory-analysis-1.00.13.md` | Avalia viabilidade de DPS real por skill/source usando `DamageInfo`, `Unit.gpw`, `ActiveSkill` e dumps Ghidra/IL2CPP. |
| Ghidra IL2CPP | `scripts/run-ghidra-il2cpp-headless.ps1`, `scripts/run-ghidra-combat-decompile.ps1`, `scripts/ghidra/` | Importa `GameAssembly.dll`, aplica labels do Il2CppDumper e exporta pseudocodigo de combate via Ghidra headless. |

## Arquitetura operacional

### Componentes principais em `src/main.cpp`

| Regiao | Responsabilidade |
| --- | --- |
| Constantes/UI IDs | Controles Win32, tray, URI Steam, caminho do save e chave ES3. |
| `Config` | Server URL, token, SteamID, pairing secret e autostart. |
| `SaveSummary` | Resumo do save e campos usados pelo sync. |
| `MemoryEvent` / `MemorySnapshot` | Eventos vivos e agregados derivados do LogManager. |
| Config/tray/UI | `LoadConfig`, `SaveConfig`, `WindowProc`, `StartWorker`, `StopWorker`. |
| JSON | Parser/serializer minimo proprio para evitar dependencias externas. |
| Save ES3 | AES-CBC/PBKDF2, parsing do JSON e construcao de resumo. |
| IL2CPP memory reader | `ReadLogManagerEvents`, offsets do mapa, classificacao de log. |
| Historico/clears | `BuildHistoryJson`, `BuildClearsJson`, caches locais. |
| Sync | `CachedPayload`, `PostJsonPayload`, `SyncCachedPayload`, sync incremental. |
| Harness CLI | `--compare`, `--dump-save-summary`, `--memory-save-scan`, `--memory-stage-scan`, `--memory-scan`. |

### Loop do worker

1. `WorkerProc` inicia e carrega `sync-state.json`.
2. `RefreshSaveCache` compara mtime do save; se mudou, le e parseia o ES3 como fallback inteiro.
3. `EnsureGameRunning` abre o TaskBarHero via Steam se o processo nao existir.
4. `RefreshLiveSaveCache` tenta ler o snapshot vivo do save em `bal` a cada ~500ms; se validar, promove `saveSource=memory`; se falhar, volta para o snapshot ES3 inteiro.
5. Se SteamID estiver vazio no config e o save ativo tiver SteamID, o agente salva automaticamente.
6. `SyncCachedPayload` envia primeiro sync ou sync forcado quando config muda; mudancas estruturais do save ativo enviam imediatamente, e mudancas metricas pendentes sao enviadas no tick de ~10s para alimentar snapshots.
7. `RefreshMemoryCache` tenta ler eventos do `LogManager` a cada ~2s.
8. Eventos novos sao adicionados com `index` monotonicamente crescente.
9. Se houve mudanca, `SyncCachedPayload` envia apenas eventos novos quando possivel.
10. Em build de desenvolvimento (`TBH_DEVELOPMENT_MODE=1`), mudancas de save ativo ou historico tambem atualizam automaticamente o runtime local do frontend.
11. O loop para quando a janela/tray manda encerrar.

### Configuracao local

Arquivo:

```text
%LOCALAPPDATA%\TBH Companion\config.ini
```

Campos importantes:

- `server`: URL base do frontend remoto.
- `token`: bearer token de ingest.
- `steam_id`: opcional; se vazio, o agente tenta ler do save.
- `pairing_secret`: senha local usada para parear browsers.
- autostart no registro do Windows.
- `app.minimize_to_taskbar`: opcional; `0` por padrao minimiza o agent para a bandeja, `1` mantem minimizacao normal na taskbar.

Regra sensivel: `pairing_secret` fica em claro no PC do usuario por escolha de produto, mas nunca deve ser enviado ao servidor. O agente envia apenas `sha256(pairing_secret)`.

### Postura read-only

O companion e somente visual/observacional em relacao ao jogo.

Regras:

- nao simular mouse, teclado ou comandos de UI do jogo;
- nao editar save oficial;
- nao mover itens, equipar, comprar, vender ou executar qualquer acao no jogo;
- pode ler o save oficial, ler eventos vivos de memoria, exportar snapshots de desenvolvimento e esconder/mostrar a janela do jogo com `ShowWindow(SW_HIDE/SW_SHOW)` pelo menu da bandeja;
- qualquer feature nova que exija interacao ativa com o jogo deve ser recusada ou redefinida como visual/read-only antes de implementacao.

### UI/tray

- O agente cria icone na bandeja e continua rodando quando a janela e fechada.
- Minimizar esconde o agente na bandeja por padrao; o menu de contexto da bandeja tem `Minimizar para taskbar` para alternar o comportamento e persistir em `config.ini`.
- O menu da bandeja inclui `Esconder TBH`/`Mostrar TBH`, que alterna a visibilidade da janela principal de `TaskBarHero.exe`; o processo do jogo continua rodando e o worker segue lendo save/memoria normalmente.
- Em build de desenvolvimento, a janela mostra `Atualizar dev` e o menu da bandeja mostra `Atualizar save local`; ambos exportam `save-summary.json`, `clears.json`, `log-history.json` e `watcher-status.json` para o runtime local. Esses controles nao existem em release.
- O label de status e um controle `STATIC` com fundo solido; ao atualizar texto menor que o anterior, force repaint para evitar sobra visual.

### Arquivos de cache/local runtime

Pasta:

```text
%LOCALAPPDATA%\TBH Companion\
```

Arquivos relevantes:

| Arquivo | Uso |
| --- | --- |
| `config.ini` | Config do agente. |
| `sync-state.json` | Progresso de sync incremental. Apagar forca reenvio completo. |
| cache de memory history | Historico local de eventos para sobreviver a reinicio. |
| cache de items extraidos | `items.json` extraido de `items.zip` embutido quando necessario. |
| arquivos temporarios de harness | Resultado de `--compare`, `--memory-save-scan`, `--memory-stage-scan`, `--memory-scan` e dump de save. |

## Contrato com o frontend

Endpoint remoto:

```http
POST <server>/api/ingest
Authorization: Bearer <token>
Content-Type: application/json
```

Payload produzido por `SavePayload` ou `CachedPayload`:

| Campo | Origem | Observacoes |
| --- | --- | --- |
| `steamId` | config ou save | Tambem usado como `siteId`. |
| `siteId` | igual ao SteamID efetivo | Chave no servidor. |
| `save` | `BuildSaveSummaryJson` | Pode ser `null` se save nao foi lido. |
| `pairingHash` | `PairingHash(config, steamId)` | Hex SHA-256 da senha local, ou `null`. |
| `clears` | `BuildClearsJson` | Agregados por stage e tentativas. |
| `history` | `BuildHistoryJson` | Eventos e summaries; pode ser parcial. |
| `watcher` | `WatcherStatusJson` | Status de fonte e timestamp. |
| `items` | dados de item embutidos/runtime | Auxilia o frontend quando aplicavel. |
| `pets` | estado de pets | Usado para UI de pets. |

Regras:

- `history.partial` significa "apenas eventos novos"; servidor mescla sem apagar.
- `EventId` precisa ser estavel. Mudar isso pode duplicar ou apagar historico.
- `event.index` define ordem; nao reordenar historico sem resetar sync.
- Eventos `clear`/`failure` devem carregar `difficulty` por evento, e `stageSummaries`/`clears.averages` devem ser agrupados por `difficulty + label`. Nao aplicar a dificuldade atual do save a todo o historico, porque ao trocar de dificuldade isso reclassifica clears antigos como se fossem da dificuldade nova.
- Se schema do payload mudar, atualizar tambem `tbh-farm-local-frontend/docs/AI_DOCUMENTATION.md`.

## Leitura do save ES3

Arquivo padrao:

```text
%USERPROFILE%\AppData\LocalLow\TesseractStudio\TaskbarHero\SaveFile_Live.es3
```

Chave:

```text
DEFAULT_ES3_KEY = "emuMqG3bLYJ938ZDCfieWJ"
```

Fluxo:

1. `ReadSaveSummary` le bytes do save.
2. `DecryptEs3AesCbc` decripta com AES-CBC e PBKDF2.
3. `LoadSaveRoot` parseia JSON.
4. `BuildSaveSummaryJson` extrai dados relevantes.
5. `SaveSummary` guarda JSON serializado e campos usados pelo worker.

Comportamento do save oficial do jogo, confirmado no dump IL2CPP 1.00.12:

- O gerenciador de save do jogo e a classe obfuscada `bal`.
- `bal.<AutoSaveAsync>d__57.MoveNext` chama `SaveAsync` e depois agenda `UniTask.Delay(TimeSpan.FromSeconds(180), ..., PlayerLoopTiming.Update, cancellationToken, ...)`.
- O autosave periodico so chama `SaveAsync` quando `UnityEngine.Time.timeScale > 0`; se o jogo estiver pausado/timeScale zero, o loop segue para o delay.
- `OnApplicationPause(true)` dispara `SaveAsync` fire-and-forget.
- `OnApplicationQuit` cancela os tokens de autosave, tenta adquirir o `SemaphoreSlim` por `3000 ms`, executa a rotina sincronizada de escrita e libera o semaforo.
- Chamadas extras para `SaveAsync` passam principalmente por `bal.mbn()` em eventos de gameplay/inicializacao: `StageManager` (spawn/recompensas/progresso), `vb.Cube` (alchemy/cube) e `vb.uj.uh` (mail/claim). Isso explica saves em menos de 180s quando o jogador recebe/claim itens, troca estado de stage ou usa sistemas que alteram inventario.
- Nao ha evidencia de UnityEvent serializado chamando diretamente os wrappers publicos de save (`dgy`, `frn`, `ljj`, `mbn`, `muj`) nos YAMLs de scene/prefab/asset do dump 1.00.12.
- No build Windows 1.00.12, `ProjectSettings.runInBackground = 1`; teste local minimizando/restaurando a janela do jogo nao mudou o `mtime` do save em 30s, entao foco/minimize nao deve ser tratado como gatilho confiavel para `OnApplicationPause(true)`.
- O caminho local `bal.SaveAsync`/`OnApplicationPause(true)` nao mostrou chamadas diretas para a pilha de backend/Steam inventory (`qj`, `wy`, `TaskbarHero.InventoryUploadToSteam*` ou `vb.Cube.Try*Backend`) na disassembly nativa. A escrita local parece ficar no ES3 protegido por `SemaphoreSlim`.
- A sincronizacao/upload de inventario/backend parece separada e concentrada em `qj.OnSteamInventoryUpdated`, `qj.SynchronizeInventoryByBackendData`, `wy.HandleBackendInventoryUpdate`, `TaskbarHero.InventoryUploadToSteam*` e nos metodos `vb.Cube.Try*Backend`. Acoes de Cube/recompensas podem acionar essa pilha por alterarem inventario; forcar apenas `OnApplicationPause(true)` deveria acionar so o save local, sem upload direto pelo jogo.
- Ressalva: Steam Cloud/cliente Steam pode sincronizar arquivos modificados fora do codigo do jogo. Isso e diferente de uma chamada de backend feita pela rotina `SaveAsync`. O manifesto local `appmanifest_3678970.acf` identifica o app como `TBH: Task Bar Hero`, mas a pasta instalada nao contem `steam_autocloud.vdf`; nao ha evidencia local de regra AutoCloud junto do build instalado.
- `scripts/test_pause_window_signals.ps1` e um harness nao invasivo para testar sinais de janela/OS (`WM_ACTIVATEAPP`, minimize/restore e opcionalmente `WM_POWERBROADCAST`) e medir se o `mtime` do save muda. Ele nao chama IL2CPP, nao injeta codigo e nao escreve memoria do processo; portanto nao equivale a chamar diretamente `bal.OnApplicationPause(bool)`.
- Teste manual do harness de sinais de janela/OS nao disparou alteracao do save. Tratamos `OnApplicationPause(true)` como inacessivel por mensagens normais de janela no Windows neste build; chamar diretamente exigiria patch/injecao/chamada interna IL2CPP.
- Varredura estatica por funcao no `GameAssembly.dll` confirmou 16 xrefs diretos para `bal.mbn()`: `StageManager.<SpawnFriendlyUnitAsync>d__115.MoveNext`, quatro callbacks `StageManager.rm` (`cxp`, `ew`, `gbt`, `ibr`), `StageManager.ru.MoveNext`, `StageManager.ry.MoveNext`, `vb.Cube.<TriggerCurrentRecipeLogic>d__138.MoveNext`, `vb.uj.uh.<TryClaimItem>d__19.MoveNext`, tres helpers de mail/claim (`dsn`, `iyo`, `kcj`) e quatro metodos de inicializacao `qi` (`ehk`, `hdi`, `ihv`, `mrj`). Nao houve xrefs diretos para `bal.dgy`, `bal.frn`, `bal.ljj` ou `bal.muj`.
- O companion nao escreve o save oficial; `RefreshSaveCache` observa apenas `mtime`. Portanto e esperado o arquivo ficar sem alteracao por mais de 1 minuto, especialmente entre ciclos de autosave de 180s ou quando a escrita esta aguardando semaforo/cancelamento/validacao.
- O ES3 agora e fallback inteiro. O snapshot preferido vem de memoria quando `ReadLiveSaveSummary` consegue ler e validar `bal`.

## Leitura viva do save pela memoria

O agente prefere ler o save vivo do singleton obfuscado `bal : np<bal>`:

- `bal.bgaw` -> `AccountSaveData`
- `bal.bgax` -> `PlayerSaveData`
- `PlayerSaveData.commonSaveData` e listas de currency, herois, atributos, pets, runas, inventario, storage, trade stash, itens e aggregates

`ReadLiveSaveSummary` monta um `JsonValue` com a mesma forma que `LoadSaveRoot` produz para o ES3 e chama o mesmo `BuildSaveSummaryJson`. Isso evita regras paralelas para equipamentos, inventario, enchants, bonuses, runas e kills.

Importante: `ReadLiveSaveSummary` le o modelo de save vivo (`bal.bgaw/bgax`) para estrutura, mas gold/EXP e o stage recem-entrado podem ficar parados ali ate o jogo comitar autosave. Por isso, antes de chamar `BuildSaveSummaryJson`, o agente aplica dados quentes de runtime sobre o root vivo: `MonsterSpawnManager.MonsterList`/`SummonedMonsterList` atualiza `CommonSaveData.currentStageKey` pelo stage carregado nos monstros vivos, `vb.tp/vb.tq` atualiza `CurrencySaveData.Quantity` do gold e `vb.tz`/`vd.beuv` atualiza `HeroSaveData.HeroExp` por `heroKey`. O ES3 continua sendo fallback inteiro quando a leitura viva nao esta disponivel. Use `--memory-stage-scan` para comparar managers runtime contra o save vivo: ele amostra `runtimeStageKey`, `DeadMonsterUnit -> Monster.cache -> MonsterInfoData`, `runtimeGold`, `runtimeHeroExp`, `saveGold` e `savePartyHeroLevels`.

Validacao local de 2026-06-18: durante 8s em combate, `runtimeGold` subiu 1444 e EXP runtime do Knight subiu 6382 enquanto `saveGold` e `savePartyHeroLevels` ficaram parados. Em seguida, `--memory-save-scan` confirmou que o resumo `memory` ja saiu com `goldDelta=5774` e EXP dos herois acima do ES3, provando que snapshots de 10s passam a ter delta real sem esperar autosave.

Validacao minima:

- processo e `GameAssembly.dll` encontrados;
- `np<bal>_TypeInfo`, `static_fields` e singleton resolvidos;
- `AccountSaveData`, `PlayerSaveData` e `CommonSaveData` nao nulos;
- listas e arrays lidos com limites maximos defensivos;
- resumo final tem SteamID valido.

Se a leitura viva falhar, `RefreshLiveSaveCache` marca `has_live_save=false`, grava `saveFallbackReason` e promove o ultimo `file_save` lido por ES3. Nao misturar blocos de memoria e arquivo no mesmo payload.

`watcher-status.json` deve incluir:

- `saveSource`: `memory`, `save` ou `none`;
- `lastSaveFileReadAt`;
- `lastLiveSaveReadAt`;
- `lastLiveSaveReadMs`;
- `saveFallbackReason` quando a leitura viva falhou.

## Tráfego do save no ingest

O contrato remoto continua aceitando `save` como snapshot completo. Nao existe `saveDelta`/patch por campo no backend.

Para reduzir banda sem criar outro protocolo, o agente envia:

- `save` completo no primeiro sync, quando uma mudanca estrutural do save ativo ocorre, quando o sync e forcado por mudanca de config, ou no tick metrico de ~10s quando houver delta de save pendente;
- `save: null` quando apenas eventos de historico mudaram. O backend preserva `tbh_current_state.save` via `coalesce(excluded.save, tbh_current_state.save)`.

Mudancas metricas continuas sao `gold`, `playTime`, `currentStageWave`, `monsterKills` e `exp` dos herois. Elas alimentam snapshots de EXP/Ouro sem transformar cada tick da memoria em POST imediato; o envio metrico e consolidado em ~10s. Mudancas estruturais continuam imediatas: stage key/max stage, inventario/storage/trade, equipamentos, skills equipadas, runas, pets, atributos e demais campos do save normalizado.

O tick de ~10s garante tentativa de envio com `save` completo apenas quando houver delta pendente. Para gold/EXP, o resumo vivo ja usa os caches runtime validados (`runtimeGold` e `runtimeHeroExp`), entao kills durante a stage geram hash novo mesmo quando o ES3 e o modelo `bal.bgaw/bgax` ainda nao foram comitados pelo autosave. Se o jogador estiver parado e o resumo efetivo for identico, o worker nao forca POST repetido so pelo tempo.

Nao enviar delta parcial de save sem antes implementar merge server-side e broadcast compatível. Varios recursos comparam save anterior vs novo (`rune alerts`, pet unlock, inventario cheio, snapshots e derived stats), entao patch parcial seria mais arriscado que economico enquanto o `save` completo ainda estiver pequeno.

Dados extraidos:

- `ownerSteamId` / `playerId`
- versao do save
- gold moeda `100001`
- stage atual e max stage
- herois e equipe ativa
- equipamentos e stats dos herois; `partyHeroLevels[].equippedItems` e `unlockedHeroes[].equippedItems` incluem os itens equipados para permitir comparacao de composicao no frontend; itens devem preservar `uniqueMod` para tooltips exibirem Stats Unicas de raridades altas
- inventario, storage e stash de troca (`inventorySaveDatas`, `stashSaveDatas`, `tradingStashSaveDatas`) com slots desbloqueados, vazios e itens resolvidos por `itemSaveDatas`
- pets e estados de unlock/viewed
- runas e niveis
- monster kills agregadas

Regras:

- Nao ressuscitar worker Python como fonte de producao.
- Se o frontend precisa de novo dado do save, implementar no C++ e validar com `--dump-save-summary`.
- Evitar parsing por string quando o `JsonValue` ja permite navegar com chaves.
- Se extrair novo array/objeto grande, considerar impacto no payload e no Postgres.
- `equipmentBonuses` agrega somente os equipamentos da equipe ativa (`partyHeroLevels`); equipamentos serializados em `unlockedHeroes` nao devem somar bonus global para evitar duplicacao e incluir herois fora da composicao atual.

## Leitura de memoria / LogManager / IL2CPP

O agente nao varre heap para achar logs. Ele le diretamente a `List<LogData>` do singleton `LogManager` usando offsets extraidos por versao do jogo.

O RVA `kLogManagerTypeInfoRva` aponta para o TypeInfo do singleton base generico de `TaskbarHero.Log.LogManager`, nao necessariamente para um simbolo literal fixo. O obfuscator pode mudar o prefixo entre builds (`nn<LogManager>_TypeInfo`, `np<LogManager>_TypeInfo`, etc.); `scripts/refresh_il2cpp_map.py` deve ler a heranca da classe no `dump.cs` e resolver o candidato correspondente em `script.json`.

Se o jogo estiver aberto mas a lista de logs ainda estiver vazia, o refresh deve aceitar a cadeia viva `static_fields/list` e preservar `kLogDataTextOffset`/`kLogDataClockOffset` atuais. Nao falhar apenas por `size=0`; isso pode acontecer em menu ou logo apos abrir o jogo.

Bloco sensivel:

```cpp
// ===== BEGIN IL2CPP MAP (TaskBarHero <versao>) =====
constexpr uintptr_t kLogManagerTypeInfoRva = ...;
constexpr uintptr_t kKlassStaticFieldsOffset = ...;
constexpr uintptr_t kLogManagerListOffset = ...;
constexpr uintptr_t kLogDataTextOffset = ...;
constexpr uintptr_t kLogDataClockOffset = ...;
constexpr uintptr_t kLogDataDateTimeOffset = ...;
constexpr uintptr_t kBoxOpenItemKeyOffset = ...;
constexpr uintptr_t kBoxOpenGradeOffset = ...;
static const char* const kGradeNames[] = { ... };
// ===== END IL2CPP MAP =====
```

Classes/eventos importantes:

| Classe/Tipo | Uso |
| --- | --- |
| `LogManager` | Singleton que segura lista de logs. |
| `StageManager` | Singleton runtime de stage; `--memory-stage-scan` amostra estado, inicio da stage e campos/listas runtime. |
| `MonsterSpawnManager` | Singleton runtime de monstros; `--memory-stage-scan` amostra listas de monstros vivos/mortos/summoned e usa monstros vivos/summoned para identificar o stage atual sem esperar autosave. |
| `Monster` / `MonsterInfoData` | `--memory-stage-scan` le os ultimos mortos via `DeadMonsterUnit`, incluindo `MonsterKey`, `RewardGold`, `RewardExp` base e o stage runtime carregado no monstro. |
| `vb.tp` / `vb.tq` | Cache runtime de currencies; `--memory-stage-scan` le `runtimeCurrencies` e `runtimeGold` sem esperar commit no save. |
| `vb.tz` / `vd` | Cache runtime de herois; `--memory-stage-scan` le `runtimeHeroExp` de `vd.beuv` sem esperar commit no save. |
| `LogData` | Texto, relogio e DateTime de cada evento. |
| `BoxOpenLog` | Item obtido; contem `ItemName_<key>` e `EGradeType`. |
| `GetBoxLog` | Bau obtido. |
| `EGradeType` | Raridade autoritativa dos drops. |

Regras:

- Se o jogo atualizar e logs falharem, rode `scripts/refresh_il2cpp_map.py` com jogo aberto.
- Sem verificacao viva (`--no-live`), o script preserva offsets que dependem de memoria viva.
- Nao inferir raridade por cor/texto quando `BoxOpenLog` fornece `grade`.
- A ordem cronologica vem da ordem de insercao da lista.
- `DateTime` interno deve alimentar `ts` real dos eventos.

## Eventos e historico

`MemoryEvent` representa evento normalizado.

Campos importantes:

- `id`: identificador estavel.
- `index`: ordem incremental local.
- `type`: clear, failure, death, drop etc.
- `category`: item, chest, material, alchemy, decoration, etc.
- `raw`: texto limpo sem markup Unity.
- `color`: cor original quando relevante.
- `grade`: raridade canonica de `EGradeType`.
- `item_key`: chave do item quando `BoxOpenLog`.
- `label`, `difficulty`, `seconds`, `clock`, `ts`.

Regras:

- Eventos de item devem cruzar `item_key` com `items.json` embutido.
- Categoria de `BoxOpenLog` vem de item type quando possivel.
- `BoxOpenLog` deve ser montado pelos campos IL2CPP antes de qualquer regex textual; nomes localizados podem conter simbolos/pontuacao como `1º`.
- `GetBoxLog` representa bau, nao item obtido.
- Eventos de "Ouro obtido" por alquimia/craft devem ser categorizados especificamente; nao transformar todo evento em craft.
- Evite heuristicas por texto quando classe IL2CPP fornece a informacao.

## Sync incremental

Estado:

```text
%LOCALAPPDATA%\TBH Companion\sync-state.json
```

Logica:

1. `SyncCachedPayload` calcula assinatura do payload efetivo enviado: save completo quando permitido, ou `save:null` quando o envio e apenas de historico.
2. Se assinatura nao mudou e nao e force sync, nao envia.
3. Se historico local continua com mesmo primeiro evento, envia de `synced_index + 1` em diante.
4. Se historico foi resetado/reordenado, reenvia tudo.
5. Ao receber HTTP 200/201, atualiza `synced_index`, `synced_first_id` e `last_payload_hash`.
6. Quando um save completo e aceito, atualiza tambem `last_synced_save_hash`, `last_synced_save_structural_hash` e o tick do ultimo sync de save. O hash estrutural ignora apenas campos metricos continuos para decidir se deve furar o throttle de 10s; o tick metrico so envia quando `save_pending` indica que o JSON efetivo mudou.

Quando forcar reenvio:

- Apagar `sync-state.json`.
- Reiniciar o agente.
- Usar quando banco foi restaurado, ids mudaram ou houve bug de dedupe.

## Items embutidos

Arquivos:

- `res/items.json`
- `res/items.zip`
- `src/generated_items.h`
- `scripts/build_items.ps1`

Fluxo de build:

1. `build.bat` chama `scripts/build_items.ps1`.
2. Script compacta `res/items.json` em `res/items.zip`.
3. Calcula SHA1 do zip.
4. Escreve `EMBEDDED_ITEMS_SHA1` em `src/generated_items.h`.
5. Resource compiler embute o zip no `.exe`.
6. Em runtime, agente extrai/cacheia conforme SHA1.

Regra:

- Nao editar `generated_items.h` manualmente.
- Se `items.json` mudar, rode `build.bat`.

## Release

Script:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\release.ps1
```

Etapas:

1. Recalcula mapa IL2CPP para a versao atual do jogo.
2. Compila `build\TBH_Companion.exe` via `build.bat --no-restart --release`, desligando `TBH_DEVELOPMENT_MODE`.
3. Detecta versao do jogo e escolhe tag.
4. Commita e faz push do agente.
5. Cria GitHub Release com `.exe` e `.zip`.

Flags:

- `-SkipMap`: usa mapa atual.
- `-NoLive`: passa `--no-live` ao map refresh.
- `-Draft`: cria release como rascunho.
- `-DryRun`: nao commita, nao envia e nao publica.
- `-GameDir`: caminho do TaskBarHero.

Pre-requisitos:

- Visual Studio Build Tools 2022.
- `git`.
- GitHub CLI `gh` autenticado.
- `py` no PATH.
- Jogo instalado; para verificacao viva, jogo aberto.

## Export Unity assets / frontend-pack

Script:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\export_unity_assets.ps1
```

Por padrao, sem flags, o script so mostra plano e nao exporta. O fluxo principal hoje e:

1. Rodar AssetRipper GUI manualmente no jogo.
2. Apontar o `ExportedProject`:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\export_unity_assets.ps1 -OrganizeExportedProject "C:\caminho\ExportedProject"
```

Saida:

```text
exported-assets\TaskBarHero-<versao>\frontend-pack\
```

Conteudo do pack:

- `images\heroes`
- `images\monsters`
- `images\items`
- `images\pets`
- `images\stages`
- `images\skills-effects`
- `images\ui`
- `data\raw-csv`
- `data\json`
- `data\localization\extracted`
- `audio`
- `contact-sheets`
- `manifest.json`

Funcoes importantes:

- `Convert-TextAssetCsvs`: converte TextAsset CSV para raw CSV e JSON.
- `Copy-PortableImages`: organiza imagens.
- `New-MonsterSpriteMap`: resolve sprites de monstros.
- `Export-LocalizationBundles`: extrai en-US e pt-BR com UnityPy.
- `New-ContactSheets`: gera folhas de contato.
- `Organize-ExportedProject`: monta o `frontend-pack`.

Observacao PowerShell: checagens de dependencia Python, como `python -c "import UnityPy"`, devem desativar temporariamente `$ErrorActionPreference = "Stop"` ou capturar o exit code explicitamente. No Windows PowerShell, stderr de processo nativo pode virar erro terminante antes do script instalar a dependencia.

Salvaguardas:

- `-EnforceVersionGuard` impede exportar duas vezes a mesma versao.
- `-Force` recria pack existente.
- `-SkipLocalizationBundleExtract` pula extracao de localization.
- Modo CLI direto existe com `-Enable -ExporterExe`, mas e experimental.

Regra:

- Assets do jogo podem ser usados no companion como fan-made; nao incluir segredos ou arquivos pessoais.
- O frontend nao deve depender da wiki se o pack contem o dado.
- Mudancas no formato do pack devem ser coordenadas com `tbh-farm-local-frontend/scripts/rebuild-from-agent-pack.ps1`.

## Referencia estatica de batalha

Objetivo:

- alimentar features visuais, como easter egg de arena no frontend, com dados extraidos do jogo em vez de heuristicas;
- manter uma base reaproveitavel para sprites, animacoes, anchors de prefab, skills, stages e classes IL2CPP relacionadas a combate.

Comando principal:

```powershell
py scripts\extract_battle_reference.py --version <versao>
```

Entradas:

- `exported-assets\TaskBarHero-<versao>\frontend-pack\manifest.json`;
- `ExportedProject` apontado pelo manifest;
- `scripts\.cache\dump\dump.cs` e `scripts\.cache\dump\script.json` gerados por `scripts\refresh_il2cpp_map.py`;
- CSVs de `Assets\TextAsset`: `HeroInfoData`, `MonsterInfoData`, `SkillInfoData`, `StageInfoData`;
- YAMLs Unity de `AnimationClip`, `AnimatorController`, `AnimatorOverrideController`, `GameObject` e `Sprite`.

Saidas:

- `docs\battle-reference-<versao>.json`: referencia estruturada para codigo;
- `docs\battle-reference-<versao>.md`: resumo legivel da extracao.

Referencia atual gerada:

```text
docs\battle-reference-1.00.13.json
docs\battle-reference-1.00.13.md
```

Regras:

- Este fluxo e read-only: nao injeta, nao chama IL2CPP e nao escreve memoria do processo do jogo.
- Para fidelidade visual, usar `HeroInfoData`/`MonsterInfoData` para prefab e animator, `SkillInfoData` para range/delivery/clips, `StageInfoData` para inimigos do stage, `UnitBaseAnimator.controller` + override controllers para estados e clips, e anchors de prefab para posicoes.
- Nao inverter sprites por suposicao; usar a escala/posicao do prefab, especialmente o objeto `View`, e os frames extraidos dos clips como referencia de orientacao.
- O C# exportado pelo AssetRipper em IL2CPP tem corpos stubados. Target selection, timing exato e fluxo interno de movimento/ataque exigem Ghidra/IDA no `GameAssembly.dll` com nomes/RVAs do Il2CppDumper.
- Para Ghidra, preferir adicionar a pasta `scripts\ghidra` no Script Manager e rodar `apply_il2cppdumper_labels.py`; o script original em `.cache\il2cppdumper\ghidra.py` pode nao aparecer porque nao tem metadados de categoria do Ghidra.

## Analise DPS real / fonte de dano

Referencia atual:

```text
docs\dps-memory-analysis-1.00.13.md
```

Conclusao para 1.00.13:

- `LogManager` nao contem eventos de dano/skill; o cache vivo observado tem apenas `clear`, `failure`, `death`, `drop` e `craft`, com campos como `type`, `category`, `hero`, `enemy`, `item`, `grade`, `itemKey`, `seconds`, `clock` e `ts`.
- `DamageInfo` contem os dados centrais para DPS: `Attacker @0x00`, `OriginDamage @0x08`, `IsCritical @0x0C`, `DamageAttribute @0x10`, `DamageType @0x14` e `HitEffects @0x20`.
- `Unit.gpw` e o ponto mais promissor para dano real aplicado: ele recebe `DamageInfo`, aplica absorcao/reducao/evasao e chama o health controller com o valor final.
- `ActiveSkill` mantem contexto de origem: `skillCache @0x18`, owner/caster `bgrq @0x38`, `AnimClipName @0x40`, `ActionTimeName @0x48`, behavior `bgrt @0x68`; `SkillBehaviorContext` tambem liga `SkillOwner`, `SkillCache` e `ActiveSkill`.
- Sem hook/instrumentacao, DPS por skill so deve ser tratado como inferencia por polling de HP, unidades e skills/projeteis ativos. DPS autoritativo exige observar a chamada quente (`Unit.gpw`, `Hero.gpw`, `Monster.gpw`, `DamageInfo.ctor` ou producers de `AttackDamage`), o que sai da postura read-only atual se exigir injecao/hook.

Regra:

- Nao adicionar DPS autoritativo ao companion de producao sem decidir explicitamente se a postura read-only continua obrigatoria. Se continuar, nomear a feature como estimativa/inferencia e validar com harness controlado.

## Movimento e ordem dos herois

Fonte de verdade para `1.00.13`:

- `scripts\.cache\dump\dump.cs` define `StatType.MovementSpeed = 7`, `StatType.AreaOfEffect = 8`, `StatType.BaseAttackCountReduction = 9` e `StatType.CooldownReduction = 10`. O agent deve manter `StatTypeName()` nessa mesma ordem.
- `HeroInfoData.MovementSpeed @0x78` e `MonsterInfoData.MovementSpeed @0x64` sao os valores base vindos dos CSVs/dados do jogo.
- No decompilado, `Hero.gqp` e `Monster` chamam `yt.kkf(cache, 7, 0)` e gravam o resultado em `Unit + 0x154..0x164`; `Unit.gsn` le exatamente esse `ObscuredFloat`.
- `StageManager.iel` percorre `StageManager.HeroList @0x30`, calcula o maior `Unit.gsn()` visto e o menor `Unit.gsn()` entre herois elegiveis, e grava a velocidade efetiva do grupo em `StageManager.bdgo @0xA0`; `StageManager.idw` e o getter direto desse campo.

Regra observada em `StageManager.iel`:

- fluxo normal: usa o menor `MovementSpeed` entre os herois elegiveis/vivos;
- excecao: se algum heroi satisfaz a checagem virtual com argumento `0xF4242`, ou se todos os herois ativos satisfazem a checagem virtual sem argumento, o valor efetivo vira o maior `MovementSpeed` do grupo;
- o decompilado nao nomeia essas chamadas virtuais (`vtable + 0x328`), entao trate a semantica exata como pendente ate mapear o metodo C# correspondente; a regra min/max acima e a parte comprovada pelo fluxo.

Ordem dos personagens:

- `CommonSaveData.arrangedHeroKey @0x48` e a ordem persistida.
- `StageManager.<SpawnFriendlyUnitAsync>d__115.MoveNext` itera `vb.tz.irq/irp` por indice, instancia o heroi com `StageManager.ifn`, grava em `HeroList[indice]`, chama `Hero.gpu(heroCache, indice)` e passa `HeroList` para `FollowCamera.eet`.
- `StageManager.ifx` troca duas entradas de `HeroList`, atualiza o indice interno do heroi (`Hero + 0x3D0`) e troca arrays auxiliares de posicao/estado (`0x148`, `0x150`, `0x158`, `0x160`).
- Conclusao: mudar a ordem muda slot, posicao horizontal, camera/UI/listas auxiliares e pode mudar indiretamente a velocidade se trocar quais herois estao ativos/elegiveis. A ordem por si so nao pesa no calculo de `MovementSpeed`; `StageManager.iel` agrega por min/max dos valores dos herois presentes.

### Ghidra CLI IL2CPP

Fluxo preferido para evitar passos manuais no Ghidra:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\run-ghidra-il2cpp-headless.ps1 -Fresh
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\run-ghidra-combat-decompile.ps1 -MaxMethods 2000
```

Arquivos envolvidos:

- `scripts\ghidra\ApplyIl2CppDumperLabelsHeadless.java`: importa `script.json` e aplica labels no projeto Ghidra.
- `scripts\ghidra\DecompileIl2CppCombatTargets.java`: decompila alvos filtrados de combate.
- `scripts\ghidra-projects-cache\TaskBarHero-Il2Cpp-1.00.13.gpr`: projeto Ghidra gerado localmente e ignorado pelo Git.
- `scripts\ghidra-projects-cache\TaskBarHero-Il2Cpp-1.00.13-report.md`: resumo de labels/RVAs.
- `scripts\ghidra-projects-cache\decompiled-combat\`: pseudocodigo C exportado por grupo (`StageManager.c`, `UnitHero.c`, `Monster.c`, `ActiveSkill.c`, `Projectiles.c`, `MonsterSpawnManager.c`).

Ultima execucao validada para `1.00.13`:

- Labels aplicados: `484257`.
- Falhas de label: `0`.
- Metodos de combate selecionados: `509`.
- Metodos de combate decompilados: `509`.
- Falhas de decompilacao: `0`.

Observacoes:

- `-Fresh` no wrapper nao passa `-deleteProject` ao Ghidra; essa flag do Ghidra cria projeto temporario e apaga no fim.
- O wrapper resolve `JAVA_HOME` automaticamente para `C:\Program Files\Eclipse Adoptium\jdk-*` quando o terminal nao tem `java` no `PATH`.
- A pasta `scripts\ghidra-projects-cache\` nao pode ficar dentro de `scripts\.cache\`, porque o Ghidra headless rejeita path com elemento iniciado por ponto.

## Atualizacao do mapa IL2CPP

Comando principal:

```powershell
py scripts\refresh_il2cpp_map.py
```

Uso com flags:

```powershell
py scripts\refresh_il2cpp_map.py --game-dir "C:\Steam\steamapps\common\TaskbarHero"
py scripts\refresh_il2cpp_map.py --dry-run
py scripts\refresh_il2cpp_map.py --no-live
py scripts\refresh_il2cpp_map.py --print-version
```

O script:

1. Localiza instalacao do jogo.
2. Detecta versao.
3. Baixa/usa Il2CppDumper.
4. Gera `dump.cs` e `script.json`.
5. Extrai RVA/offsets de `LogManager`, `LogData`, `BoxOpenLog`, save vivo (`bal`), `StageManager`, `MonsterSpawnManager` e enum `EGradeType`.
6. Com jogo aberto, valida na memoria viva.
7. Patcha:
   - `src/main.cpp`
   - `../tbh-farm-local-frontend/server.js`
   - `../tbh-farm-local-frontend/public/calculator.js`

Regra critica:

- Se o enum `EGradeType` ganhar grade novo, atualizar `GRADE_PT` no script.
- Nao aceitar offset novo sem harness quando o jogo esta aberto e a verificacao viva e possivel.
- Se usar `--no-live`, registre o risco: offsets dependentes de memoria podem ter sido preservados da versao anterior.

## Ambiente e comandos

Build:

```bat
build.bat
```

Saida:

```text
build\TBH_Companion.exe
```

Comparar leitura de save:

```powershell
build\TBH_Companion.exe --compare
```

Dump do resumo C++:

```powershell
build\TBH_Companion.exe --dump-save-summary C:\temp\tbh-save-summary.json
```

Comparar save vivo em memoria contra ES3:

```powershell
build\TBH_Companion.exe --memory-save-scan C:\temp\tbh-memory-save-scan.json
```

Scan runtime de stage/monstros contra save vivo:

```powershell
build\TBH_Companion.exe --memory-stage-scan "$env:TEMP\tbh-memory-stage-scan.json" 30
```

Scan de memoria:

```powershell
build\TBH_Companion.exe --memory-scan
```

Release dry-run:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\release.ps1 -DryRun
```

Export plan:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\export_unity_assets.ps1
```

Organizar ExportedProject:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\export_unity_assets.ps1 -OrganizeExportedProject "C:\caminho\ExportedProject"
```

## Harnesses de validacao

### Harness minimo para C++

```bat
build.bat
```

Validar:

- `build\TBH_Companion.exe` existe.
- `scripts\build_items.ps1` gerou `res\items.zip` e `src\generated_items.h`.
- Nao ha erro de linker por executavel em uso.

### Harness de save

```powershell
build\TBH_Companion.exe --compare
build\TBH_Companion.exe --dump-save-summary "$env:TEMP\tbh-save-summary.json"
build\TBH_Companion.exe --memory-save-scan "$env:TEMP\tbh-memory-save-scan.json"
build\TBH_Companion.exe --memory-stage-scan "$env:TEMP\tbh-memory-stage-scan.json" 30
```

Validar:

- Exit code 0.
- JSON gerado parseia.
- `ownerSteamId`/`playerId` esperado.
- `currentStageKey`, `maxStageKey`, gold, herois, runas, pets e monster kills aparecem.
- Com jogo aberto, `--memory-save-scan` deve retornar `liveOk=true`; diferencas pontuais podem ser esperadas quando a memoria ja tem mudancas que o ES3 ainda nao salvou.
- `--memory-stage-scan` deve retornar amostras com ponteiros de `StageManager`/`MonsterSpawnManager`, contagens de listas de monstros, `runtimeStageKey`, `deadMonsterRewardTotals`, `recentDeadMonsters`, `runtimeGold`, `runtimeHeroExp` e, no mesmo sample, `saveGold`/`savePartyHeroLevels`. Use este harness para provar se runtime muda antes do save vivo.
- Se uma feature frontend depende de novo campo, confirmar o campo no dump.

### Harness de memoria/log

Com jogo aberto:

```powershell
build\TBH_Companion.exe --memory-scan
```

Validar:

- PID do jogo encontrado.
- `history.events` contem eventos recentes.
- `clears` contem stage summaries quando ha clear/failure.
- Drops de item tem `item_key` e `grade`.
- `watcher` reporta `cpp-agent il2cpp reader` e, quando gerado pelo worker/export dev, inclui `saveSource`.

### Harness de mapa IL2CPP

```powershell
py scripts\refresh_il2cpp_map.py --dry-run
py scripts\refresh_il2cpp_map.py
build.bat
build\TBH_Companion.exe --memory-save-scan C:\temp\tbh-memory-save-scan.json
build\TBH_Companion.exe --memory-scan
```

Validar:

- Script detecta versao correta.
- Dump gera offsets.
- Verificacao viva mostra amostras de eventos.
- Saida do script mostra `Save manager: TypeInfo np<bal>_TypeInfo`.
- Saida do script mostra `Runtime stage` com TypeInfos de `StageManager` e `MonsterSpawnManager`.
- Saida do script mostra `Runtime rewards` com offsets de `Monster.cache`, `MonsterInfoData.RewardGold` e `MonsterInfoData.RewardExp`.
- Saida do script mostra `Runtime currency` com TypeInfo de `vb.tp`, offset da lista de currencies e offset do `ObscuredLong` usado por `vb.tq`.
- Saida do script mostra `Runtime heroes` com TypeInfo de `vb.tz`, offset do dicionario de herois e offset do `ObscuredFloat` usado por `vd.beuv`.
- `src/main.cpp` foi patchado dentro dos marcadores.
- Frontend `server.js` e `public/calculator.js` recebem mapa de raridade se mudou.

### Harness de sync remoto

Use somente com token real em ambiente seguro.

1. Configure `server`, `token`, `steam_id` e `pairing_secret` na UI do agente.
2. Inicie o jogo.
3. Inicie o agente.
4. Observe status: save lido, jogo encontrado, eventos novos, sync OK.
5. Abra `https://tbh.gided.in/<steamId>`.
6. Confirme atualizacao no frontend.

Se precisar reenvio completo:

```powershell
Remove-Item "$env:LOCALAPPDATA\TBH Companion\sync-state.json"
```

### Harness de assets

```powershell
powershell -ExecutionPolicy Bypass -File scripts\export_unity_assets.ps1 -OrganizeExportedProject "C:\caminho\ExportedProject"
```

Validar:

- `frontend-pack\manifest.json` existe.
- `data\raw-csv` e `data\json` existem.
- `data\localization\extracted` contem `en-US` e `pt-BR`.
- `images\heroes`, `images\monsters`, `images\items`, `images\pets` tem arquivos esperados.
- `contact-sheets` foi gerado ou warning documentado.
- Depois, no frontend, rodar `npm run build:from-agent`.

### Harness de release

Antes de release real:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\release.ps1 -DryRun
```

Para release:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\release.ps1
```

Validar:

- `gh auth status` OK.
- Mapa atualizado.
- Build OK.
- Tag calculada.
- Commit/push do agente OK.
- GitHub Release criado.
- Asset `.exe` e `.zip` anexados.

## Skills operacionais

### Skill: corrigir leitura de save

1. Leia `ReadSaveSummary`, `ReadLiveSaveSummary`, `BuildSaveSummaryJson` e helpers de JSON.
2. Gere dump com `--dump-save-summary` antes de editar, se possivel.
3. Adicione o campo no resumo sem quebrar campos existentes; se vier do save, implemente no caminho ES3 e no caminho vivo por memoria.
4. Se o frontend depende do novo campo, documente e ajuste o frontend.
5. Rode `build.bat`, `--dump-save-summary` e `--memory-save-scan` com jogo aberto.

### Skill: corrigir eventos/historico

1. Leia `ReadLogManagerEvents`, `MemoryEvent`, `EventJson`, `BuildHistoryJson`.
2. Confirme se a classe IL2CPP fornece dado autoritativo.
3. Evite heuristica de texto/cor se houver campo de classe ou item.
4. Preserve `id` e `index`.
5. Rode `--memory-scan` com jogo aberto.
6. Verifique frontend Historico e Stages.

### Skill: atualizar para nova versao do jogo

1. Rode `py scripts\refresh_il2cpp_map.py --dry-run`.
2. Rode sem dry-run com jogo aberto.
3. Build com `build.bat`.
4. Rode `--memory-save-scan`, `--memory-stage-scan` e `--memory-scan`.
5. Se grade novo aparecer, atualize traducoes no script e no frontend.
6. Reinicie agente.
7. Se necessario, apague `sync-state.json` para reenvio completo.

### Skill: alterar payload de sync

1. Leia `SavePayload`, `CachedPayload`, `SyncCachedPayload`.
2. Preserve `history.partial` e semantica incremental.
3. Inclua novo campo de forma retrocompativel.
4. Ajuste o servidor frontend em `POST /api/ingest`.
5. Rode harness de save, memory scan e ingest.
6. Atualize docs dos dois repos.

### Skill: alterar pairing/auth

1. Leia `PairingHash` e campos de config.
2. Nunca enviar senha em claro.
3. Se mudar derivacao, alterar tambem frontend/server.
4. Testar pareamento em browser.
5. Confirmar que alteracao de senha dispara novo sync mesmo sem reiniciar o agente.

### Skill: alterar export de assets

1. Leia `export_unity_assets.ps1` e `extract_unity_localization.py`.
2. Use AssetRipper GUI + `-OrganizeExportedProject` como fluxo principal.
3. Mantenha `frontend-pack` estavel para o frontend.
4. Se criar nova categoria, atualizar `Ensure-PortableTree` e manifest.
5. Gerar contact sheets quando possivel.
6. Rodar rebuild no frontend.

### Skill: release

1. Confirme working tree.
2. Rode `scripts\release.ps1 -DryRun`.
3. Rode release real apenas quando build/mapa estiverem validados.
4. Nao esconder falhas de `gh`, `git`, `build.bat` ou map refresh.
5. Depois do release, validar download do GitHub Release.

## Seguranca e governanca

Nunca documentar em commits:

- token de ingest;
- senha de pairing;
- senha de servidor/SSH;
- credenciais GitHub;
- dumps de save reais com dados pessoais;
- URLs privadas com token;
- chaves VAPID privadas.

Dados sensiveis:

- `config.ini`;
- `sync-state.json`;
- save ES3;
- payloads completos de `/api/ingest`;
- SteamID/ownerSteamId;
- eventos de historico do jogador.

Regras:

- O agente roda localmente no PC do usuario; senha em claro no `config.ini` e aceitavel por decisao de produto, mas nao deve sair da maquina.
- Apenas hash SHA-256 da senha e enviado.
- Token de ingest autentica o agente contra o servidor; trate como segredo.
- Logs de debug nao devem imprimir token nem senha.
- Se publicar artefatos, publicar apenas `.exe`, `.zip` e docs seguras.

## Uso de subagentes

Use subagentes quando a tarefa envolver risco ou muitos modulos:

- Um subagente analisa `src/main.cpp` save/sync.
- Outro analisa IL2CPP/map refresh.
- Outro analisa assets/export.
- Outro revisa contrato com frontend.

Subagentes de pesquisa devem receber escopo claro e pedido explicito para nao editar arquivos. A IA principal consolida e implementa.

## Antipadroes a evitar

- Misturar Python abandonado com implementacao runtime do agente.
- Assumir que offsets IL2CPP continuam validos apos update do jogo.
- Inferir raridade por cor quando `EGradeType` existe.
- Enviar senha de pairing em claro.
- Resetar sync-state sem avisar que reenvia historico completo.
- Editar `generated_items.h` manualmente.
- Rodar export Unity com `-Force` sem checar path/versao.
- Criar dependencia externa no C++ sem necessidade; o agente e Win32 puro.
- Refatorar `main.cpp` inteiro em tarefa pequena.
- Comitar dumps/export bruto gigantes sem intencao clara.

## Checklist de PR/commit

- Resumo da mudanca.
- Dominio afetado: save, memoria, sync, UI/tray, items, IL2CPP, assets ou release.
- Arquivos alterados.
- Afeta contrato com frontend?
- Harness executado.
- Jogo aberto foi usado quando precisava de live memory?
- Segredos/dumps foram evitados?
- Docs atualizadas?
- Precisa release?

## Metricas recomendadas

| Metrica | Como medir | Sinal positivo |
| --- | --- | --- |
| Tempo para update de jogo | Do release do jogo ao agente funcional | Menor tempo. |
| Falhas de memory scan | Quantidade por versao | Menos falhas apos map refresh. |
| Precisao de eventos | Comparar frontend vs log in-game | Menos eventos faltantes/duplicados. |
| Payload incremental | Bytes/eventos por sync | Menor payload sem perda de dados. |
| Erros de ingest | Respostas HTTP nao 200/201 | Menos falhas. |
| Fallbacks tecnicos no frontend | Busca por nomes crus apos rebuild | Menos fallback. |
| Releases refeitos | Tags `-1`, `-2` por mesma versao | Menos retrabalho. |

## Politica de manutencao

- Se mudar save parsing, payload, IL2CPP map, release, export Unity ou contrato com frontend, atualize este arquivo.
- Se a doc divergir do codigo, o codigo vence e a doc deve ser corrigida.
- Revisar apos cada update relevante do TaskBarHero.
- Manter este documento operacional, com caminhos reais e comandos reais.
- Nao transformar este arquivo em historico de tarefas temporarias.
