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
- Salva automaticamente URL do servidor, token, SteamID e inicializacao com Windows.
- Le o save ES3 padrao e monta o mesmo resumo gerado pelo worker Python.
- Embute `items.json` como `items.zip` no exe e extrai para cache por SHA1 quando necessario.
- Abre o jogo pelo Steam se `TaskBarHero.exe` nao estiver rodando.
- Monitora o save por timestamp e so relê quando o arquivo muda.
- Monitora logs da memoria do `TaskBarHero.exe`, mantendo cache das regioes candidatas para reduzir custo.
- Scan de memoria otimizado: busca multi-padrao via `memchr`, janelas de contexto mescladas (regex roda uma vez por trecho) e passe rapido apenas em memoria privada gravavel (heap gerenciado), com fallback para scan completo.
- Faz POST para `/api/ingest` apenas quando o payload muda.
- Pode ser configurado para iniciar automaticamente com o Windows.
- Abre a UI web do SteamID configurado.
