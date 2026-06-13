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
