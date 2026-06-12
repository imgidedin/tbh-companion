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

MVP atual:

- GUI nativa Win32.
- Salva URL do servidor, token e SteamID.
- Faz POST de teste para `/api/ingest`.
- Abre a UI web do SteamID configurado.

Proximas etapas:

- Ler SteamID diretamente do save.
- Portar decrypt do save.
- Portar watcher de memoria/logs.
