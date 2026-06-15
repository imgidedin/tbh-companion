# Inventory -> Storage Automation Plan

Este documento acompanha a tarefa de automatizar, no agente C++, a movimentacao de itens do inventario para o storage do TaskBarHero usando a UI real do jogo. O objetivo e nao tocar no save e nao implementar frontend enquanto a execucao local nao estiver confiavel.

## Objetivo

Mover itens do inventario para o storage de forma automatica, segura e validavel:

- o agente escolhe um item do inventario;
- confirma que ha storage livre;
- abre/foca a UI do jogo quando necessario;
- executa o gesto real do jogo;
- valida antes/depois por save e/ou memoria viva;
- futuramente aceita tarefas remotas do frontend, mas isso fica fora desta fase.

## Regras de seguranca

- Nunca editar `SaveFile_Live.es3` para mover item.
- Nao implementar fila/frontend ate a automacao local ser confiavel.
- Se o jogo/slot/UI nao estiver em estado esperado, abortar com status claro.
- Preferir dados vivos IL2CPP sobre coordenadas fixas ou heuristica por pixel.
- Toda descoberta que virar requisito deve entrar neste documento e no `AI_DOCUMENTATION.md` quando afetar comportamento do agente.

## Ja feito

- Confirmado no dump do jogo que `ESlotAction.MoveToStash = 4`.
- Confirmado no dump/AssetRipper que `m_moveToStashKey = 306`, ou seja `LeftControl`.
- Implementado botao local `Mover p/ bau` no `companion.exe`.
- A primeira implementacao valida pelo save se ha item no inventario e slot livre no storage.
- A primeira implementacao foca `TaskBarHero.exe`, espera 3s e envia `LeftControl + clique direito` no item sob o cursor.
- Teste manual confirmou que `Ctrl + right click` move o item sob o mouse para o storage.
- Documentado em `docs/AI_DOCUMENTATION.md` que a versao atual ainda depende do cursor.

## Estado atual

Funciona como prova de conceito local quando o usuario posiciona o cursor sobre o item no inventario do jogo. Ainda nao move automaticamente um slot escolhido pelo agente, porque ainda falta mapear coordenadas vivas dos slots Unity.

## Plano macro

### 1. Mapear UI viva do inventario/storage

Meta: conseguir, a partir do processo vivo do jogo, listar slots de inventario/storage e obter coordenada de tela para clicar.

Subtarefas:

- [x] Resolver `SlotInteractionManager` vivo via singleton IL2CPP.
- [x] Confirmar se o campo `SlotInteractionManager.bddd` (`Dictionary<GameObject, qr>` em `0x80`) contem os slots atualmente registrados.
- [x] Enumerar o dictionary e filtrar os objetos cuja classe indique `InventorySlot` ou `StashSlot`.
- Para cada slot vivo, ler:
  - [ ] tipo (`ESlotType.INVENTORY = 1`, `ESlotType.STASH = 2`) sem depender so do nome da classe;
  - [x] indice (`InventorySlot.index @ 0xA0`, `StashSlot.Index @ 0x78`);
  - [x] item/unique id, quando disponivel (`ItemSlot.bfqq @ 0x28` ou campo proprio do slot);
  - [x] `RectTransform` do slot (`ItemSlot.m_slotRectTransform @ 0x38`).
- [ ] Converter o `RectTransform`/canvas em coordenada de cliente Win32.
- [ ] Validar em jogo aberto com inventario visivel: centro calculado deve cair sobre o slot correto.

Achados iniciais:

| Entidade | Dado util |
| --- | --- |
| `np<SlotInteractionManager>_TypeInfo` | RVA `0x5E1C4F8` na versao `1.00.12`. E o melhor ponto de entrada para singleton. |
| `SlotInteractionManager` | Herda de `np<SlotInteractionManager>` e tem `canvas @ 0x30`, `m_moveToStashKey @ 0x50`, `bddd @ 0x80`, `MovingSlotOffset @ 0xA0`. |
| `SlotInteractionManager.bddd` | `Dictionary<GameObject, qr>`; candidato principal para enumerar slots vivos registrados. |
| `System.Collections.Generic.Dictionary<GameObject, qr>_TypeInfo` | RVA `0x5DBBD08`; util para validar layout do dictionary em memoria. |
| `qr` | Interface de slot; expoe `hpp()` tipo, `hpq()` indice, `hps()` `RectTransform`, `hpu()` unique id, mas nao vamos invocar metodos nesta fase. |
| `ItemSlot` | Base de `InventorySlot`/`StashSlot`; tem `bfqq @ 0x28`, `m_slotRectTransform @ 0x38`. |
| `InventorySlot` | Tem `index @ 0xA0`, `bfqp @ 0xB0`; `hpp()` retorna `INVENTORY`. |
| `StashSlot` | Tem `Index @ 0x78`, `bfqy @ 0xB0`; `hpp()` retorna `STASH`. |
| `UI_Hero` | Tem `inventorySlots @ 0x78` e `bcyp @ 0x80`; rota alternativa se o dictionary global nao bastar. |
| `UI_RemakeStash` | Tem `m_stashSlotList @ 0x60`, `bfsx @ 0x78`, `bfta @ 0x90`, tab atual `bftd @ 0xC0`; rota alternativa para storage. |
| AssetRipper `GameScene.unity` | Confirma `m_moveToStashKey: 306`, `m_showDirectMoveRudderTime: 0.3`, `MovingSlotOffset: {-7, 7}`. |
| `scripts/probe_live_slots.py` | Harness de desenvolvimento criado para validar slots vivos antes de portar para C++. |

Resultado do probe ao vivo em `TaskBarHero.exe` com a UI aberta:

- `SlotInteractionManager` resolvido em memoria via `np<SlotInteractionManager>_TypeInfo`, com `static_fields_offset=0xB8`.
- `bddd Dictionary<GameObject, qr>` resolvido e enumerado.
- Layout do dictionary confirmado com `entry stride = 0x18`.
- 33 slots vivos encontrados no estado testado:
  - 6 `InventorySlot`;
  - 25 `StashSlot`;
  - 2 `CubeInventorySlot`.
- Para `InventorySlot` e `StashSlot`, o probe leu corretamente `index`, ponteiro de `RectTransform` e `ItemSlot.bfqq`.
- Coordenada de tela ainda nao foi calculada; este e o restante real do passo 1.

Decisao tecnica atual:

- Primeiro tentar a rota `SlotInteractionManager` -> `bddd` -> slots registrados.
- So recorrer a `UI_Hero`/`UI_RemakeStash` se o dictionary nao listar slots fora da tela, ou se o dictionary nao for estavel.
- Para coordenadas, o caminho mais provavel e usar uma chamada controlada aos metodos Unity exportados (`Transform.get_position`, `RectTransform.get_rect`, `RectTransformUtility.WorldToScreenPoint`) ou uma funcao equivalente injetada/local no processo. Ler so campos gerenciados nao basta, porque `RectTransform` e `Transform` guardam a geometria no objeto nativo apontado por `UnityEngine.Object.m_CachedPtr`.
- Nao portar para C++ ate o harness conseguir imprimir coordenadas clicaveis e elas forem conferidas visualmente.

Riscos em aberto:

- Layout real de `Dictionary<GameObject, qr>` em IL2CPP foi confirmado no probe atual, mas precisa ser revalidado a cada versao do jogo.
- `RectTransform` para coordenada de tela exige acessar geometria nativa Unity, nao apenas campos IL2CPP gerenciados.
- Storage pode ter paginas/scroll; mover para storage com `Ctrl+right click` deixa o jogo escolher o destino, entao inicialmente so precisamos clicar no slot de origem do inventario.
- Slots nao visiveis ou UI fechada devem abortar, nao tentar clicar em coordenada stale.

Resultado esperado do passo 1:

- Uma rotina de debug local no agente ou harness CLI que liste slots vivos:

```text
INVENTORY index=0 uid=... rect=(x,y,w,h) screen=(x,y)
INVENTORY index=1 empty rect=(x,y,w,h) screen=(x,y)
STASH index=... rect=(x,y,w,h) screen=(x,y)
```

### 2. Mover item de origem calculada

Meta: substituir o cursor manual por clique calculado.

- Escolher o primeiro slot ocupado do inventario pelo save/memoria.
- Encontrar o slot vivo correspondente por indice.
- Clicar no centro do slot com `LeftControl + right click`.
- Validar que o comando foi enviado apenas se o slot vivo ainda corresponde ao item esperado.

### 3. Validar antes/depois

Meta: saber se o jogo realmente executou a acao.

- Ler snapshot de inventario/storage antes.
- Executar o gesto.
- Aguardar save/memoria refletir alteracao.
- Confirmar que o `itemUniqueId` saiu do inventario e apareceu em storage.
- Se falhar, reportar motivo: UI fechada, item mudou, storage cheio, slot nao encontrado, jogo sem foco, save nao atualizou.

### 4. Automatizar abertura/estado da UI

Meta: reduzir pre-condicoes manuais.

- Descobrir atalho/fluxo para abrir inventario/storage se fechado.
- Detectar se UI de inventario esta visivel.
- Se necessario, abrir a UI antes de tentar mapear slot.

### 5. Fila local/remota

Fora da etapa atual.

- Frontend cria tarefa por SteamID autenticado.
- Backend armazena fila.
- Agent busca tarefas, valida estado local, executa e reporta status.
- Operacao suportada: somente inventario -> storage.

## Proxima acao

Concluir o harness de leitura viva do passo 1:

- resolver uma forma segura de obter o centro de cada `RectTransform` em coordenada de tela;
- imprimir `screen=(x,y)` para cada `InventorySlot`;
- conferir visualmente se `InventorySlot index=0`, `index=1`, etc. apontam para os slots corretos;
- so depois disso portar o leitor minimo para C++ e trocar o clique no cursor pelo clique calculado.
