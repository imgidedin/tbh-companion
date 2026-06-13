# TBH Companion Agent

Worker leve em C++ Win32 puro para configurar e enviar dados ao servidor do companion.

## Build

```bat
build.bat
```

Saida:

```text
build\TBH_Companion.exe
```

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

Opcoes: `--game-dir "caminho\TaskbarHero"`, `--no-live` (so dump, preserva os
offsets que dependem de verificacao viva), `--dry-run` (so mostra o que mudaria).
Depois: `build.bat`, reinicie o agente e suba o frontend. Se o enum de raridade
ganhar um grade novo, o script avisa para preencher a traducao PT em `GRADE_PT`.

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
