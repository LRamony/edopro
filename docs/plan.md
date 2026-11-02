# Plano de Implementação: Legendas Automáticas de Cartas (10 Idiomas)

## Visão Geral

Implementar sistema de tradução automática de cartas do Yu-Gi-Oh no EDOPro que:
1. **Busca traduções oficiais** da API YGOPRODeck (5 idiomas suportados nativamente)
2. **Traduz via LibreTranslate** para idiomas não cobertos (5 idiomas restantes)
3. **Armazena em banco locale** (`config/languages/{locale}/cards.cdb`) para performance
4. **Funciona on-demand** - traduz apenas quando a carta é visualizada
5. **Cache local** - evita re-traduções e chamadas de API desnecessárias

---

## Idiomas Alvo (10)

| Código | Idioma | Fonte Primária | Fallback |
|--------|--------|----------------|----------|
| `en` | Inglês | YGOPRODeck (nativo) | - |
| `pt-BR` | Português Brasil | YGOPRODeck (`pt`) | LibreTranslate |
| `fr` | Francês | YGOPRODeck (`fr`) | LibreTranslate |
| `de` | Alemão | YGOPRODeck (`de`) | LibreTranslate |
| `it` | Italiano | YGOPRODeck (`it`) | LibreTranslate |
| `zh-CN` | Chinês Mandarim | - | LibreTranslate |
| `es` | Espanhol | - | LibreTranslate |
| `ar` | Árabe | - | LibreTranslate |
| `bn` | Bengali | - | LibreTranslate |
| `ru` | Russo | - | LibreTranslate |
| `ur` | Urdu | - | LibreTranslate |

**Nota**: YGOPRODeck usa `pt` para português (pt-BR/pt-PT). Usaremos `pt-BR` como locale no EDOPro.

---

## Arquitetura do Sistema

```
Game::ShowCardInfo()
         │
         ▼
TranslationManager (Novo)
  ├── Cache Local (SQLite)
  ├── YGOPRODeck API (5 idiomas)
  └── LibreTranslate (5 idiomas)
         │
         ▼
Locale Database (cards.cdb)
config/languages/{locale}/cards.cdb
```

---

## Componentes a Implementar

### 1. TranslationCache (SQLite Local)
**Arquivo**: `gframe/translation_cache.h/cpp`

```cpp
// Schema do cache
CREATE TABLE translations (
    card_id INTEGER NOT NULL,
    locale TEXT NOT NULL,
    source TEXT NOT NULL,
    name TEXT,
    desc TEXT,
    str1 TEXT, ..., str16 TEXT,
    updated_at INTEGER NOT NULL,
    PRIMARY KEY (card_id, locale)
);

CREATE TABLE api_usage (
    date TEXT PRIMARY KEY,
    ygoprodeck_calls INTEGER DEFAULT 0,
    libretranslate_chars INTEGER DEFAULT 0
);
```

### 2. YGOPRODeckClient
**Arquivo**: `gframe/ygoprodeck_client.h/cpp`

- HTTP client assíncrono (libcurl)
- Rate limiting: 20 req/s
- Cache de resposta (2 dias conforme API)
- Suporta parâmetro `&language={code}`

### 3. LibreTranslateClient
**Arquivo**: `gframe/libretranslate_client.h/cpp`

- HTTP client para LibreTranslate
- Suporta auto-hospedado ou instância pública
- Batch translation para múltiplos textos
- Rate limiting configurável

### 4. TranslationManager (Orquestrador)
**Arquivo**: `gframe/translation_manager.h/cpp`

```cpp
class TranslationManager {
public:
    bool EnsureCardTranslated(uint32_t cardCode, const std::string& locale);
    
    // Fluxo:
    // 1. Verifica cache local (SQLite)
    // 2. Se não tem, verifica locale DB (cards.cdb)
    // 3. Se não tem, busca YGOPRODeck (se idioma suportado)
    // 4. Se não tem, traduz via LibreTranslate
    // 5. Salva no locale DB e cache
    
    struct Stats { int cached; int api_calls; int translations_done; };
    Stats GetStats() const;
    
private:
    TranslationCache cache;
    YGOPRODeckClient ygoprodeck;
    LibreTranslateClient libretranslate;
    std::string current_locale;
};
```

### 5. Integração no Game::ShowCardInfo
**Arquivo**: `gframe/game.cpp` (modificação)

```cpp
void Game::ShowCardInfo(uint32_t code, bool resize, imgType type, bool forcePersistent) {
    // ... código existente ...
    
    // NOVO: Garantir tradução para locale atual
    if (gGameConfig->locale != "en") {
        std::string locale = Utils::ToUTF8IfNeeded(gGameConfig->locale);
        gTranslationManager->EnsureCardTranslated(code, locale);
    }
    
    // O GetText/GetName já pega do locale DB automaticamente
    stName->setText(gDataManager->GetName(tmp_code).data());
    stText->setText(gDataManager->GetText(code).data());
}
```

### 6. Inicialização do Sistema de Locale
**Arquivo**: `gframe/data_handler.cpp` (modificação)

```cpp
void DataHandler::DataHandler() {
    // ... código existente ...
    
    // NOVO: Inicializar TranslationManager
    gTranslationManager = std::make_unique<TranslationManager>();
    gTranslationManager->Initialize(gGameConfig->locale);
    
    LoadDatabases();
    // ...
}
```

---

## Fluxo de Tradução (On-Demand)

```
Usuário passa mouse sobre carta (código: 12345678)
                    │
                    ▼
Game::ShowCardInfo(12345678)
                    │
                    ▼
TranslationManager::EnsureCardTranslated(12345678, "pt-BR")
                    │
        ┌───────────┴───────────┐
        ▼                       ▼
   Cache tem?              Locale DB tem?
        │                       │
       Sim                     Sim
        │                       │
        └───────────┬───────────┘
                    ▼
           Retorna tradução
                    │
                    ▼
         Atualiza UI com texto traduzido
```

**Se NÃO tem em nenhum dos dois:**

```
Busca YGOPRODeck (se pt, fr, de, it, en)
        │
   ┌────┴────┐
   ▼         ▼
  Tem     Não tem
   │         │
   ▼         ▼
Salva    LibreTranslate
no DB    (traduz EN→PT)
   │         │
   └────┬────┘
        ▼
   Salva no Locale DB + Cache
        │
        ▼
   Retorna tradução
```

---

## Modificações no Banco de Dados Locale

### Estrutura Atual (ParseLocaleDB)
```sql
CREATE TABLE texts (
    id INTEGER PRIMARY KEY,
    name TEXT,
    desc TEXT,
    str1 TEXT, ..., str16 TEXT
);
```

### Inserção de Tradução
```cpp
bool TranslationManager::SaveToLocaleDB(uint32_t code, const CardTranslation& t) {
    // Abre/cria ./config/languages/{locale}/cards.cdb
    // INSERT OR REPLACE INTO texts (id, name, desc, str1...str16) VALUES (...)
    // Fecha DB
}
```

---

## Configuração (system.conf / user_configs)

```json
{
  "translation": {
    "enabled": true,
    "ygoprodeck": {
      "enabled": true,
      "rate_limit_per_sec": 20
    },
    "libretranslate": {
      "enabled": true,
      "url": "https://libretranslate.de/translate",
      "api_key": "",
      "rate_limit_per_min": 60
    },
    "cache": {
      "max_age_days": 30,
      "max_entries": 50000
    }
  }
}
```

---

## UI - Aba de Configurações (Cliente → Idioma)

```cpp
// Combo box para seleção de idioma de tradução
tabSettings.cbTranslationLang = env->addComboBox(GetNextRect(), tabPanel, COMBOBOX_TRANSLATION_LANG);
tabSettings.cbTranslationLang->addItem(L"Desativado", -1);
tabSettings.cbTranslationLang->addItem(L"Português Brasil (pt-BR)", 0);
// ... outros idiomas
tabSettings.cbTranslationLang->setSelected(gGameConfig->translation_lang);

// Checkbox para ativar/desativar
tabSettings.chkAutoTranslate = env->addCheckBox(gGameConfig->auto_translate, GetNextRect(), tabPanel, CHECKBOX_AUTO_TRANSLATE, gDataManager->GetSysString(XXXX).data());
```

---

## Estrutura de Arquivos a Criar/Modificar

### Novos Arquivos
```
gframe/
├── translation_cache.h/cpp
├── ygoprodeck_client.h/cpp
├── libretranslate_client.h/cpp
├── translation_manager.h/cpp
├── card_translation.h
```

### Arquivos Modificados
```
gframe/
├── game.cpp
├── game.h
├── data_handler.cpp
├── data_manager.h/cpp
├── game_config.inl
├── premake5.lua
```

---

## Build System (premake5.lua)

```lua
files {
    "gframe/translation_cache.cpp",
    "gframe/ygoprodeck_client.cpp",
    "gframe/libretranslate_client.cpp",
    "gframe/translation_manager.cpp",
}
```

Links necessários: `curl`, `sqlite3` (já linkados no projeto)

---

## Rate Limiting & Etiqueta

### YGOPRODeck
- **20 req/segundo**
- Cache local de 2 dias
- User-Agent: `EDOPro/41.0.0 (Translation System)`

### LibreTranslate
- Público: ~100 req/hora por IP
- Auto-hospedado: sem limite
- Token bucket local

---

## Testes e Validação

### Cenários de Teste
1. Primeira visualização - Carta nunca traduzida → API → DB → UI
2. Segunda visualização - Carta no cache → Cache → UI (instantâneo)
3. Troca de idioma - Limpa locale strings, recarrega do novo locale DB
4. Carta sem tradução oficial - YGOPRODeck falha → LibreTranslate → DB
5. Offline - Usa apenas cache/DB local
6. Rate limit - Queue com backoff exponencial

### Métricas de Sucesso
- Latência 1ª vez: < 2s (API) / < 500ms (LibreTranslate)
- Latência cache: < 10ms
- Taxa de acerto cache: > 90% após 1 hora de uso
- Zero crashes por falha de tradução

---

## Riscos e Mitigações

| Risco | Probabilidade | Impacto | Mitigação |
|-------|---------------|---------|-----------|
| YGOPRODeck muda API | Média | Alto | Versionamento no client, fallback gracioso |
| LibreTranslate público indisponível | Baixa | Médio | Suporte a instância local (Docker) |
| Rate limit atingido | Média | Baixo | Queue local + backoff + cache agressivo |
| Tradução ruim (LibreTranslate) | Média | Baixo | Permitir edição manual + reportar |
| DB locale corrompido | Baixa | Alto | Transações SQLite + backup automático |
| Memória alta (cache) | Baixa | Médio | LRU eviction + limite configurável |

---

## Cronograma Estimado

| Fase | Tarefa | Tempo |
|------|--------|-------|
| 1 | TranslationCache (SQLite) | 2 dias |
| 2 | YGOPRODeckClient (HTTP + parsing) | 2 dias |
| 3 | LibreTranslateClient | 1 dia |
| 4 | TranslationManager (orquestração) | 2 dias |
| 5 | Integração Game::ShowCardInfo | 1 dia |
| 6 | UI Configurações (Cliente → Idioma) | 1 dia |
| 7 | Testes + Ajustes | 2 dias |
| **Total** | | **~11 dias** |

---

## Exemplo de Uso Final

```cpp
// Usuário define nas configurações: "Português Brasil (pt-BR)"
// Inicia jogo, abre deck builder, passa mouse sobre "Dark Magician"

// 1. ShowCardInfo(46986414) chamado
// 2. TranslationManager vê locale = "pt-BR"
// 3. Verifica cache: MISS
// 4. Verifica ./config/languages/pt-BR/cards.cdb: MISS
// 5. Chama YGOPRODeck: GET /api/v7/cardinfo.php?id=46986414&language=pt
// 6. Recebe: {"name": "Mago Negro", "desc": "O mago definitivo..."}
// 7. Salva no locale DB (INSERT INTO texts...)
// 8. Salva no cache SQLite
// 9. UI mostra "Mago Negro" + descrição em PT-BR

// Próxima vez: Cache HIT → Instantâneo (0ms)
```

---

## Próximos Passos

1. **Aprovar este plano** ✓
2. **Configurar instância LibreTranslate** (Docker: `docker run -d -p 5000:5000 libretranslate/libretranslate`)
3. **Implementar Fase 1-2** (Cache + YGOPRODeck)
4. **Testar com 1 idioma** (pt-BR)
5. **Expandir para 10 idiomas**
6. **Adicionar UI de configuração**

---

## Observações Importantes

1. **strings.conf**: O YGOPRODeck **não** tem strings do sistema. Recomendo criar `strings.conf` traduzido manualmente para cada locale (one-time effort).

2. **Criação automática de arquivos**: Se `config/languages/{locale}/` não existe, criar na primeira tradução.

3. **Compatibilidade**: O sistema de locale do EDOPro já suporta múltiplos `cards.cdb` por idioma. Nossa implementação apenas **popula** esses bancos.

4. **Gratuito**: Ambos os serviços são gratuitos. LibreTranslate auto-hospedado = custo zero infinito.

---

*Documento gerado para implementação no EDOPro v41.0.0+*