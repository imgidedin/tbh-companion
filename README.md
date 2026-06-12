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
- Salva URL do servidor, token e SteamID.
- Le o save ES3 padrao e monta o mesmo resumo gerado pelo worker Python.
- Abre o jogo pelo Steam se `TaskBarHero.exe` nao estiver rodando.
- Monitora o save por timestamp e so relê quando o arquivo muda.
- Monitora logs da memoria do `TaskBarHero.exe`, mantendo cache das regioes candidatas para reduzir custo.
- Faz POST para `/api/ingest` apenas quando o payload muda.
- Pode ser configurado para iniciar automaticamente com o Windows.
- Abre a UI web do SteamID configurado.
