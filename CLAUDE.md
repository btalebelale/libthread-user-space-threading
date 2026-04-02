# 🛠️ PROJET : Bibliothèque de Threads Utilisateur — Performance Bare-Metal

**Objectif** : Threads utilisateur en C, performance nanoseconde, zéro abstraction superflue.

## 🎯 NIVEAU DE PERFORMANCE ATTENDU

Le code de ce projet doit atteindre le niveau d'optimisation des fichiers de référence
dans `reference/`. Ces fichiers illustrent :

- mmap direct sur fd avec pointeur brut (pas de FILE*, pas de fread)
- Parsing SIMD (SSE2/AVX2) : `_mm_maddubs_epi16` → `_mm_madd_epi16` → `_mm_mul_epu32` pipeline
- Output via `write(2)` syscall avec buffer d'entiers et LUT 4 bytes précalculées
- Traitement par blocs cache-friendly (ex: BLOCK = 1 << 14)
- Zéro allocation dynamique dans les hot paths

**Chaque primitive de la lib (create, yield, join, exit) doit être optimisée
avec la même rigueur que le parsing I/O de ces fichiers de référence.**

## 🧠 PRINCIPES D'OPTIMISATION — PAR ORDRE DE PRIORITÉ

### 1. Éliminer les couches d'abstraction

- `mmap(MAP_PRIVATE | MAP_ANONYMOUS)` pour les stacks, pas `malloc`
- `mprotect(PROT_NONE)` pour les guard pages
- Syscalls directs (`clone`, `futex`) si justifié par un benchmark
- Pas de `ucontext_t` en production — context switch en assembleur inline

### 2. Maîtriser le layout mémoire

- `alignas(64)` sur toute structure accédée fréquemment (cache line = 64 bytes)
- Séparer hot data / cold data dans des structs distinctes

```c
// HOT : accédé à chaque schedule()
struct thread_hot {
    void *rsp;           // 8B
    uint32_t state;      // 4B
    uint32_t priority;   // 4B
} __attribute__((aligned(64)));

// COLD : accédé rarement
struct thread_cold {
    void *stack_base;
    size_t stack_size;
    char name[32];
};
```

- Tableau de structs (AoS) vs struct de tableaux (SoA) : choisir selon le pattern d'accès
- Prefetch explicite : `__builtin_prefetch(&ready_queue[next], 0, 3)`

### 3. Exploiter le compilateur et le CPU

- `#pragma GCC optimize("Ofast,unroll-loops")`
- `#pragma GCC target("avx2")` si SIMD utilisé
- `[[likely]]` / `[[unlikely]]` sur chaque branche du scheduler
- `__attribute__((always_inline))` sur les fonctions < 10 lignes du hot path
- `__attribute__((noinline, cold))` sur les chemins d'erreur
- `__builtin_expect`, `__builtin_ctz`, `__builtin_clz`, `__builtin_popcount`
- `[[assume(expr)]]` (C++23) ou `__builtin_unreachable()` pour guider l'optimiseur

### 4. Structures de données internes

- Ready queue : bitmap hiérarchique (cf. O(1) scheduler Linux)

```c
uint64_t level0;              // 1 mot : 64 groupes
uint64_t level1[64];          // chaque bit = 1 priorité avec threads prêts
// schedule() = __builtin_ctzll(level0) → __builtin_ctzll(level1[g]) → index
```

- Hash map interne : open addressing, Fibonacci hashing
  (`key * 11400714819323198485ULL >> shift`), linear probing
- Aucune liste chaînée dans le hot path (cache hostile)

### 5. SIMD quand justifié

- Scanner un tableau de statuts : `_mm256_cmpeq_epi32` + `_mm256_movemask_ps`
- Copie de contexte : `_mm256_store_si256` / `_mm256_load_si256`
- LUT via `_mm_shuffle_epi8` (pshufb) pour les conversions/lookups
- Toujours fournir un fallback scalaire (`#ifdef __AVX2__` / `#else`)

## 🔬 MÉTHODOLOGIE — CHAQUE OPTIMISATION DOIT ÊTRE PROUVÉE

```bash
# Mesurer AVANT d'optimiser
perf stat -e cycles,instructions,cache-misses,branch-misses ./bench_yield

# Vérifier le code généré
objdump -d -M intel libthread.o | less
# ou
gcc -S -O3 -mavx2 -masm=intel scheduler.c -o scheduler.s
```

```c
// Micro-benchmark avec rdtsc
static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (uint64_t)hi << 32 | lo;
}
```

**Cycle de travail** :

1. Écrire le code le plus simple qui fonctionne
2. Écrire un micro-benchmark isolé
3. `perf stat` → identifier le bottleneck
4. Optimiser **uniquement** le bottleneck identifié
5. `objdump -d` → vérifier que le compilateur fait ce qu'on veut
6. Re-benchmarker → confirmer l'amélioration
7. Commit

## 📝 CONVENTIONS DE CODE

### Commentaires : expliquer POURQUOI, pas QUOI

```c
// Fibonacci hashing : distribution quasi-uniforme, 1 mul + 1 shift
// vs modulo : division coûteuse (~35 cycles sur Zen3)
#define HASH(k) ((k) * 11400714819323198485ULL >> SHIFT)
```

### Magic numbers interdits

Tout doit être un `#define` ou `static const` nommé.

### LUT documentées

Un commentaire au-dessus de chaque LUT expliquant l'encodage
(cf. les commentaires dans les fichiers de référence — même niveau de détail).

### Debug conditionnel

```c
#ifdef NDEBUG
#define DBG_ASSERT(expr) [[assume(expr)]]
#else
#define DBG_ASSERT(expr) assert(expr)
#endif
```

## 📂 STRUCTURE (À ne pas prendre ne compte pour l'instant)

```
├── include/
│   └── thread.h            # API publique uniquement
├── src/
│   ├── context_switch.S    # Assembleur pur, System V ABI
│   ├── scheduler.c         # Hot path, -Ofast
│   ├── stack.c             # mmap/mprotect
│   └── thread.c            # Création/destruction
├── bench/
│   ├── bench_yield.c
│   ├── bench_create.c
│   └── bench_join.c
├── reference/
│   ├── fast_io_hashmap.cpp
│   └── simd_aplusb.cpp
└── Makefile
```

## ⛔ ANTI-PATTERNS — NE JAMAIS FAIRE

- `malloc` / `free` dans un hot path
- `printf` / `fprintf` dans un hot path (utiliser `write(2)` + buffer si besoin de debug I/O)
- Linked list pour la ready queue
- `ucontext_t` en production (trop lent : sauvegarde les signaux)
- Branches non annotées dans le scheduler
- Struct non alignée accédée en boucle

## 🤖 INSTRUCTIONS COMPORTEMENTALES POUR L'AGENT

- **Code incrémental** : Ne pas tenter de tout coder d'un coup. Isoler la logique, générer le code d'un module, tester via le terminal, corriger les erreurs, puis passer à la suite.
- **Benchmark avant optimisation** : Toute optimisation proposée doit être accompagnée d'une mesure avant/après.
- **Lire le désassemblage** : Quand une optimisation est appliquée sur un hot path, vérifier avec `objdump -d` ou `gcc -S` que le résultat est conforme aux attentes.
- **Fichiers de référence** : Consulter `reference/` comme étalon de style et de rigueur. Le code produit doit atteindre ce niveau de qualité.
- **Fallback scalaire** : Tout code SIMD doit compiler et fonctionner sans AVX2 (`#ifdef __AVX2__`).
*$