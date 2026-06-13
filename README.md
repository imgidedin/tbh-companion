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
- Monitora logs da memoria do `TaskBarHero.exe`, mantendo cache das regioes candidatas para reduzir custo.
- Scan de memoria otimizado: busca multi-padrao via `memchr`, janelas de contexto mescladas (regex roda uma vez por trecho) e passe rapido apenas em memoria privada gravavel (heap gerenciado), com fallback para scan completo.
- Eventos ordenados cronologicamente: ordenacao estavel por posicao no texto + relogio `[HH:MM]` (com tratamento de virada de meia-noite); o historico mesclado preserva indices ja atribuidos (append-only).
- Sync incremental: faz POST para `/api/ingest` apenas quando o payload muda, enviando somente os eventos novos (`history.partial`); o servidor mescla por id e nunca apaga eventos ja registrados. O progresso fica em `%LOCALAPPDATA%\TBH Companion\sync-state.json` — apague esse arquivo para forcar um reenvio completo (ex.: apos restaurar o banco do servidor).
- Pode ser configurado para iniciar automaticamente com o Windows.
- Abre a UI web do SteamID configurado.
