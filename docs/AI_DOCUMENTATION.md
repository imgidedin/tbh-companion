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
- extrai resumo de save com gold, stage atual, herois, itens equipados, runas, pets e monster kills;
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
| Harness CLI | `--compare`, `--dump-save-summary`, `--memory-scan`. |

### Loop do worker

1. `WorkerProc` inicia e carrega `sync-state.json`.
2. `RefreshSaveCache` compara mtime do save; se mudou, le e parseia o save.
3. `EnsureGameRunning` abre o TaskBarHero via Steam se o processo nao existir.
4. Se SteamID estiver vazio no config e o save tiver SteamID, o agente salva automaticamente.
5. `SyncCachedPayload` envia primeiro sync ou sync forcado quando config muda.
6. `RefreshMemoryCache` tenta ler eventos do `LogManager` a cada ~2s.
7. Eventos novos sao adicionados com `index` monotonicamente crescente.
8. Se houve mudanca, `SyncCachedPayload` envia apenas eventos novos quando possivel.
9. O loop para quando a janela/tray manda encerrar.

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

Regra sensivel: `pairing_secret` fica em claro no PC do usuario por escolha de produto, mas nunca deve ser enviado ao servidor. O agente envia apenas `sha256(pairing_secret)`.

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
| arquivos temporarios de harness | Resultado de `--compare`, `--memory-scan` e dump de save. |

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

Dados extraidos:

- `ownerSteamId` / `playerId`
- versao do save
- gold moeda `100001`
- stage atual e max stage
- herois e equipe ativa
- equipamentos e stats dos herois
- pets e estados de unlock/viewed
- runas e niveis
- monster kills agregadas

Regras:

- Nao ressuscitar worker Python como fonte de producao.
- Se o frontend precisa de novo dado do save, implementar no C++ e validar com `--dump-save-summary`.
- Evitar parsing por string quando o `JsonValue` ja permite navegar com chaves.
- Se extrair novo array/objeto grande, considerar impacto no payload e no Postgres.

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

1. `SyncCachedPayload` calcula assinatura com save, quantidade de eventos, ultimo event id e pairing hash.
2. Se assinatura nao mudou e nao e force sync, nao envia.
3. Se historico local continua com mesmo primeiro evento, envia de `synced_index + 1` em diante.
4. Se historico foi resetado/reordenado, reenvia tudo.
5. Ao receber HTTP 200/201, atualiza `synced_index`, `synced_first_id` e `last_payload_hash`.

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
2. Compila `build\TBH_Companion.exe`.
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
5. Extrai RVA/offsets de `LogManager`, `LogData`, `BoxOpenLog` e enum `EGradeType`.
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
build\TBH_Companion.exe --dump-save-summary C:\temp\tbh-save-summary.json
```

Validar:

- Exit code 0.
- JSON gerado parseia.
- `ownerSteamId`/`playerId` esperado.
- `currentStageKey`, `maxStageKey`, gold, herois, runas, pets e monster kills aparecem.
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
- `watcher` reporta `cpp-agent il2cpp reader`.

### Harness de mapa IL2CPP

```powershell
py scripts\refresh_il2cpp_map.py --dry-run
py scripts\refresh_il2cpp_map.py
build.bat
build\TBH_Companion.exe --memory-scan
```

Validar:

- Script detecta versao correta.
- Dump gera offsets.
- Verificacao viva mostra amostras de eventos.
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

1. Leia `ReadSaveSummary`, `BuildSaveSummaryJson` e helpers de JSON.
2. Gere dump com `--dump-save-summary` antes de editar, se possivel.
3. Adicione o campo no resumo sem quebrar campos existentes.
4. Se o frontend depende do novo campo, documente e ajuste o frontend.
5. Rode `build.bat` e `--dump-save-summary`.

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
4. Rode `--memory-scan`.
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
