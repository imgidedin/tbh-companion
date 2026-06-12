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
- Le o save ES3 padrao e extrai SteamID, gold, stage atual/max, pet ativo, pets e runas.
- Faz POST parcial para `/api/ingest` com o resumo real do save.
- Abre a UI web do SteamID configurado.

Proximas etapas:

- Portar watcher de memoria/logs.
- Expandir o resumo do save para herois/equipamentos quando substituirmos o worker Python.
