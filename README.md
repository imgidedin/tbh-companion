# TBH Companion Agent

Worker leve em C++ Win32 puro para configurar e enviar dados ao servidor do companion.

## Build

```bat
build.bat
```

O build fecha uma instancia local de `build\TBH_Companion.exe`, recompila e
inicia o executavel novo ao terminar. Para compilar sem relancar o app:

```bat
build.bat --no-restart
```

Saida:

```text
build\TBH_Companion.exe
```

## Release (build + publicar no GitHub)

Para gerar e publicar uma versao nova de uma vez (com o jogo aberto, para a
verificacao do mapa na memoria viva):

```powershell
powershell -ExecutionPolicy Bypass -File scripts\release.ps1
```

O `release.ps1`:

1. Recalcula o mapa IL2CPP (`refresh_il2cpp_map.py`) para a versao atual do jogo.
2. Garante que existe `exported-assets\TaskBarHero-<versao>\frontend-pack\`; se faltar, exige `-ExportedProject` para organizar o export do AssetRipper antes de continuar.
3. Roda o rebuild do repo `tbh-farm-local-frontend`, commita e faz push das mudancas de dados/assets do jogo.
4. Fecha uma instancia de dev rodando em `build\`, compila o `TBH_Companion.exe` e relanca o executavel novo ao final.
5. Descobre a versao do jogo e monta a tag: `<versao>`; se ja existir um release dessa versao, usa `<versao>-1`, `<versao>-2`, ...
6. Commita as mudancas e envia para o `origin` (https://github.com/imgidedin/tbh-companion.git, adicionado automaticamente se faltar).
7. Cria o **GitHub Release** na tag com `TBH_Companion.exe` e um `.zip` anexados para download.

Flags: `-SkipMap` (so build+release), `-NoLive` (nao le a memoria do jogo),
`-Draft` (release como rascunho), `-DryRun` (faz tudo local sem commitar/enviar/publicar),
`-GameDir "caminho\TaskbarHero"`, `-FrontendDir "caminho\tbh-farm-local-frontend"`,
`-ExportedProject "caminho\ExportedProject"` quando o pack da versao ainda nao existe,
`-ForceAssetExport` para recriar o pack, `-NoContactSheets`, `-SkipFrontend` para
release apenas do agente, e `-LogPath "caminho\release.log"`.
Sem `-LogPath`, o script grava um transcript em `dist\release-<timestamp>.log`.
Requer `git`, `gh` (autenticado: `gh auth login`), `py`, Node/npm no PATH e o repo
`tbh-farm-local-frontend` disponivel no caminho padrao ou em `-FrontendDir`.

## Atualizar o mapa IL2CPP (versao nova do jogo)

O agente le os eventos direto da memoria do jogo usando RVAs/offsets especificos
da versao (bloco `IL2CPP MAP` em `src/main.cpp`). Quando sai uma versao nova,
rode **com o jogo aberto**:

```bat
py scripts\refresh_il2cpp_map.py
```

O script: localiza o jogo (Steam) e a versao, baixa/usa o Il2CppDumper, gera o
dump, extrai os offsets (LogManager, LogData, BoxOpenLog, enum `EGradeType`),
**verifica na memoria viva** (resolve a cadeia de ponteiros, descobre o offset de
`static_fields` por forca bruta, confirma texto/relogio/DateTime e mostra uma
amostra dos ultimos eventos) e atualiza automaticamente, entre marcadores:

- `src/main.cpp` (bloco `IL2CPP MAP` + `kGradeNames`)
- `../tbh-farm-local-frontend/server.js` (`gradePt`)
- `../tbh-farm-local-frontend/public/calculator.js` (`HISTORY_GRADE_PT`)

> Dica: o `release.ps1` ja roda este passo. Use o comando acima sozinho quando
> quiser so atualizar o mapa sem gerar um release.

Por padrao, o companion compara o `Version.txt` instalado com a versao compilada
no bloco `IL2CPP MAP` e bloqueia abertura/sync se elas divergirem. Para
diagnostico, marque **Permitir versão divergente** na janela do companion.

Opcoes: `--game-dir "caminho\TaskbarHero"`, `--no-live` (so dump, preserva os
offsets que dependem de verificacao viva), `--dry-run` (so mostra o que mudaria).
Depois: `build.bat`, reinicie o agente e suba o frontend. Se o enum de raridade
ganhar um grade novo, o script avisa para preencher a traducao PT em `GRADE_PT`.

## Atualizar runtime local do frontend

Em builds locais de desenvolvimento, a janela do `TBH_Companion.exe` mostra o
botao **Atualizar dev** e o menu de bandeja mostra **Atualizar save local**.
Essas opcoes exportam os dados locais usados pelo frontend em modo dev e
substituem os arquivos:

- `save-summary.json`
- `clears.json`
- `log-history.json`
- `watcher-status.json`

O destino segue esta ordem:

1. `TBH_DEV_RUNTIME_DIR`, se a variavel de ambiente estiver definida.
2. Uma pasta irma `tbh-farm-local\runtime`, encontrada subindo a partir do
   caminho do executavel do agente.
3. `%LOCALAPPDATA%\TBH Companion\runtime`, como fallback.

A mesma rotina pode ser chamada por terminal com:

```bat
build\TBH_Companion.exe --export-dev-runtime
```

Em builds de desenvolvimento, o menu de bandeja tambem mostra o toggle
**Atualizar dev automaticamente**. Quando ele esta ativo, o worker atualiza
esses arquivos automaticamente ao detectar mudanca no save ou novos eventos de
historico.
Builds gerados por `scripts\release.ps1` desligam esse modo e nao exibem esses
controles.

Se nao houver historico vivo nem cache valido, a rotina falha sem sobrescrever a
runtime com `null`.

## Exportar assets Unity (dev/manual)

Para extrair sprites/artwork do jogo para uma arvore local facil de portar para
o frontend, exporte o jogo pelo AssetRipper GUI e depois organize o
`ExportedProject` gerado:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\export_unity_assets.ps1 -OrganizeExportedProject "C:\caminho\ExportedProject"
```

O script gera `exported-assets\TaskBarHero-<versao>\frontend-pack\`, com:

- `images\`: PNGs separados em heroes, pets, monsters, items, stages,
  skills-effects, ui, fonts e misc.
- `data\raw-csv\`: CSVs originais extraidos de `TextAsset`.
- `data\json\`: JSONs dos dados principais (`HeroInfoData`, `MonsterInfoData`,
  `PetInfoData`, `StageInfoData`, `ItemInfoData`, etc.).
- `data\localization\extracted\`: traducoes oficiais extraidas dos bundles
  Addressables do jogo (`en-US` e `pt-BR`) em JSON e TSV.
- `audio\`: sons extraidos de `AudioClip`.
- `contact-sheets\`: folhas de contato para revisar visualmente os assets.

Por padrao, sem flags, ele so detecta o jogo/versao e mostra o plano:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\export_unity_assets.ps1
```

A trava para nao exportar duas vezes a mesma versao esta pronta, mas desligada
por enquanto. Quando quiser validar esse comportamento, adicione
`-EnforceVersionGuard`; se precisar sobrescrever uma exportacao existente, use
tambem `-Force`.

Durante `scripts\release.ps1`, este fluxo e chamado automaticamente quando o
`frontend-pack` da versao atual ainda nao existe e `-ExportedProject` foi
informado. Sem pack existente e sem `-ExportedProject`, o release falha antes de
publicar para evitar agente e frontend fora de sincronia.

Tambem existe um modo CLI experimental (`-Enable -ExporterExe ...`), mas o fluxo
principal hoje e AssetRipper GUI + `-OrganizeExportedProject`.

A extracao de localization usa Python + `UnityPy` para ler os bundles em
`TaskBarHero_Data\StreamingAssets\aa\StandaloneWindows64`. O script tenta achar
Python automaticamente e instala `UnityPy` se faltar. Se precisar apontar um
Python especifico, use `-PythonExe`; para pular essa etapa temporariamente, use
`-SkipLocalizationBundleExtract`.

## Config

O app salva configuracao em:

```text
%LOCALAPPDATA%\TBH Companion\config.ini
```

Atual:

- GUI nativa Win32.
- Roda na bandeja do sistema: inicia oculto quando ja configurado, fechar a janela apenas esconde, menu de contexto com Configuracoes / Abrir painel web / Sair.
- Salva automaticamente URL do servidor, token, SteamID e inicializacao com Windows.
- Le o save ES3 padrao e monta o mesmo resumo gerado pelo worker Python.
- Embute `items.json` como `items.zip` no exe e extrai para cache por SHA1 quando necessario.
- Abre o jogo pelo Steam se `TaskBarHero.exe` nao estiver rodando.
- Monitora o save por timestamp e so relê quando o arquivo muda.
- Le os eventos de log direto da `List<LogData>` do `LogManager` do jogo, via cadeia de ponteiros do mapa IL2CPP (RVAs/offsets extraidos uma vez por versao com o Il2CppDumper a partir de `GameAssembly.dll` + `global-metadata.dat`). Leitura completa em ~0,5s, sem varrer heap.
- Ordem cronologica garantida pelo proprio jogo (ordem de insercao da lista) e timestamp real (`ts`, epoch) por evento, vindo do `DateTime` interno de cada log.
- Eventos enxutos: `raw` limpo (sem markup do Unity), cor da raridade extraida no campo `color`, ids curtos (`tipo|item|relogio|ts`).
- Categoria e raridade autoritativas: a classe IL2CPP de cada log identifica o evento. `BoxOpenLog` (item obtido) carrega a chave do item (`ItemName_<key>`, cruzada com `items.json` para definir `category` = equipment/material/chest) e o `EGradeType` real (`grade` = COMMON..COSMIC); `GetBoxLog` = baus. Sem depender de heuristica por nome/cor (o frontend mapeia `grade` para o nome PT-BR do jogo).
- Sync incremental: faz POST para `/api/ingest` apenas quando o payload muda, enviando somente os eventos novos (`history.partial`); o servidor mescla por id e nunca apaga eventos ja registrados. O progresso fica em `%LOCALAPPDATA%\TBH Companion\sync-state.json` — apague esse arquivo para forcar um reenvio completo (ex.: apos restaurar o banco do servidor).
- Se o jogo atualizar e a leitura falhar ("Leitura do log falhou" no log), rode o Il2CppDumper de novo e atualize os RVAs/offsets em `src/main.cpp` (constantes `kLogManager*`/`kLogData*`); confira tambem os offsets de `BoxOpenLog`/`GetBoxLog` (campos `@0x40`/`@0x48`) e o enum `EGradeType` (`GradeTypeName`).
- Pode ser configurado para iniciar automaticamente com o Windows.
- Abre a UI web do SteamID configurado.
