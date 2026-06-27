# Sprint Change Proposal — Recentrer la route post-Epic-4 : Epic 6 = « save/recall des sessions », browser postponé

**Date:** 2026-06-27
**Author:** Antho (validator) + Dev (Claude)
**Trigger context:** Epic 4 (Phase 3 — transport sync + système d'items) vient de passer `done`. Aucune story en vol. Les epics restants (5, 6, 7) sont `backlog`.
**Scope classification:** **Moderate** — réorganisation de backlog (re-séquençage + report d'epics) + recentrage de scope d'Epic 6 + 2 reformulations de spec (FR32, FR46). **Aucun rollback, aucune invalidation de travail terminé** — Epics 1–4 (Phases 0.5 → 3) restent `done` et intouchés.

---

## Section 1 — Issue Summary

Avec Epic 4 livré, l'utilisateur dépose un fichier d'animation **directement sur une piste** (drag-drop → media item → `GetCurrentAnimItem` → load paresseux). Ce **système d'items natif** rend le **browser in-Reaper (Epic 5)** secondaire : sa raison d'être — « placer une animation sans quitter Reaper » — est déjà couverte par le drag-drop d'items.

**Décisions (Antho, 2026-06-27) :**

1. **Reporter Epic 5 (browser ReaImGui)** en post-release. Le système d'items le rend secondaire ; il ne bloque pas le ship.
2. **Recentrer Epic 6** sur **uniquement** « sauver correctement les sessions Reaper et les recall correctement » — la persistance qui fait qu'un projet rouvert retrouve chaque item et son binding. Le reste du scope actuel d'Epic 6 (reload, validation FBX/Collada, dégradation gracieuse) est reporté avec le browser.
3. **Epic 7 (ReaPack ship) reste valide et inchangé** — c'est le gate de release.

**Sur la portabilité des chemins (point soulevé par Antho) :** un media item est censé être traité nativement par Reaper comme du média référencé par chemin, donc copié/relinké localement dans le projet. Plutôt qu'un moteur de remap maison (le plan d'origine de FR46), **on s'appuie sur ce comportement natif** — **à vérifier en condition réelle**, et à rendre natif si Reaper ne le fait pas spontanément pour nos sources.

**Évidence technique (relevée dans le code, 2026-06-27) :**
- `AnimSource::GetFileName()` et `SetFileName()` **sont déjà implémentés** ([src/pcm_source_anim.cpp:68-74](src/pcm_source_anim.cpp#L68-L74)) → nos items sont **file-backed** : Reaper dispose du chemin pour ses opérations natives de copie-média et de relink. La portabilité native est donc **plausible sans code**, juste à valider.
- `SaveState()` / `LoadState()` **sont des stubs vides** ([src/pcm_source_anim.cpp:84-85](src/pcm_source_anim.cpp#L84-L85), marqués `// stub — D9 / Epic 6`) → **aujourd'hui rien n'est persisté**. C'est le vrai trou fonctionnel : sans round-trip, un projet rouvert ne sait plus quel fichier chaque item référence. **C'est précisément le cœur d'Epic 6 recentré.**

---

## Section 2 — Impact Analysis

### Epic Impact

| Epic | État actuel | Après changement |
|------|-------------|------------------|
| **1–4** | done | **inchangés** (aucun rollback) |
| **5 — Browser in-Reaper** | backlog (MVP) | **POSTPONÉ post-release** (après Epic 7). Stories 5.1–5.3 conservées telles quelles, juste re-séquencées. |
| **6 — « Reliable across sessions… »** | backlog, scope large (persistance + reload + validation FBX/Collada + remap multi-machines + dégradation gracieuse) | **REFACTORÉ → « Save & recall des sessions Reaper »**. Scope réduit à la persistance round-trip + portabilité native. |
| **7 — ReaPack ship** | backlog | **inchangé** — reste le gate de release. |
| **8 — (NOUVEAU) Robustesse, reload & couverture formats** | — | **NOUVEAU bucket post-release** qui récupère ce qui sort d'Epic 6 (reload, dégradation gracieuse, validation FBX 60 %/embedded + Collada). |

**Nouvel ordre d'exécution (le numéro ≠ l'ordre désormais) :**
`… Epic 6 (save/recall) → Epic 7 (RELEASE) → [post-release] Epic 5 (browser) + Epic 8 (robustesse/formats)`

### Artifact Conflicts

- **PRD** — aucune FR supprimée ; deux FR **reformulées** (mécanisme changé) + re-séquençage des FR vers les epics reportés :
  - **FR32** (dock position) : actuellement « *delegated to ReaImGui's own state persistence* ». Or ReaImGui a été reporté (Spike 0 → notre panneau est une **fenêtre GL dockée**, pas un panneau ReaImGui). Ce mécanisme est **orphelin**. → Reformuler : persistance de la position de dock via **l'état dock/screenset natif de Reaper** (ou notre ext-state), pas ReaImGui.
  - **FR46** (portabilité multi-machines) : actuellement « *relative path + missing-media remap* » (moteur maison). → Reformuler : **s'appuyer sur la copie/relink média natifs de Reaper** (`GetFileName`/`SetFileName` déjà en place) ; **vérifier d'abord**, implémenter le natif seulement si absent. Pas de moteur de remap maison.
  - FR3/FR19/FR47 (validation FBX/Collada, textures FBX embarquées), FR34/FR35/FR36/FR37 (dégradation, diagnostics, reload, isolation) : **non supprimées**, re-rattachées à Epic 8 (post-release).
  - FR42–FR45 + FR30/AR3 (browser + ReaImGui) : re-rattachées à Epic 5 (postponé).

- **Architecture** — entrée **AR20 Spec Change Log** requise (date 2026-06-27), corrigeant deux dérives :
  - **D9 — persistance** ([architecture.md:46](_bmad-output/planning-artifacts/architecture.md#L46), [:77](_bmad-output/planning-artifacts/architecture.md#L77), [:323-327](_bmad-output/planning-artifacts/architecture.md#L323-L327), [:824](_bmad-output/planning-artifacts/architecture.md#L824), [:1003](_bmad-output/planning-artifacts/architecture.md#L1003)) : la ligne « panel dock position → ReaImGui's own state » devient « → **dock/screenset natif Reaper** » (ReaImGui hors MVP).
  - **FR46 / Phase 4** ([architecture.md:491](_bmad-output/planning-artifacts/architecture.md#L491)) : « relative path + missing-media remap » → « **portabilité par copie/relink média natifs Reaper**, vérifiée en condition réelle ». Le moteur de remap maison sort du scope MVP.
  - **Phase mapping** ([architecture.md:489-491](_bmad-output/planning-artifacts/architecture.md#L489-L491)) : Phase 4 redéfinie = « save/recall des sessions » (persistance + portabilité native) ; reload/validation/dégradation → post-release.

- **Epics** — `epics.md` : Epic 5 annoté POSTPONÉ ; Epic 6 réécrit (titre + scope + 3 stories) ; nouvel Epic 8 ajouté.

- **Sprint status** — `sprint-status.yaml` : Epic 6 re-storifié, Epic 8 ajouté, notes de re-séquençage. (Mise à jour à l'étape handoff après ton approbation.)

- **Hors impact** : code des Epics 1–4 (aucun touché), CMake, renderer, transport. Le seul code qu'Epic 6 activera (`SaveState`/`LoadState`) était **déjà** prévu pour Epic 6 (D9) — ce n'est pas du scope nouveau, c'est le scope **restant** d'Epic 6.

---

## Section 3 — Recommended Approach

**Option retenue : Direct Adjustment + re-séquençage de backlog (Hybrid léger).** Effort **Low**, risque **Low**.

- Pas de rollback (Option 2 rejetée — rien de terminé n'est en cause).
- Pas de réduction du MVP *produit* (Option 3 partielle) : on ne **supprime** aucune capacité, on **reporte** le browser et la robustesse/validation en post-release pour atteindre le ship plus vite avec le cœur « les projets survivent au save/reopen ».
- **Justification :** le système d'items (Epic 4) a déplacé la valeur. Le chemin critique vers un outil interne shippable est maintenant *« je rouvre mon projet et tout est là »* (Epic 6 recentré) **puis** *« je l'installe via ReaPack »* (Epic 7). Le browser et le durcissement formats sont du confort post-release pour un outil interne — cohérent avec la philosophie « substance avant cérémonie ».

### Epic 6 refactoré — « Save & recall des sessions Reaper »

> **Epic 6: Save and recall Reaper sessions correctly**
> Persister l'état par-item et l'état du panneau dans le fichier projet Reaper via `SaveState`/`LoadState` (D9), de sorte qu'un projet rouvert — y compris sur une autre machine — retrouve chaque item d'animation, son binding fichier et l'état du viewport, **sans reconfiguration manuelle ni ré-import forcé**. La portabilité des chemins s'appuie sur le traitement média natif de Reaper, pas sur un remap maison.
> **FRs couverts :** FR31, FR32 (reformulé), FR33, FR46 (reformulé). *(Phase 4, recentrée.)*

- **Story 6.1 — Persister & restaurer l'état par-item au save/load projet** (FR31, FR33, D9)
  Implémenter `AnimSource::SaveState`/`LoadState` : round-trip du chemin fichier (+ offset/scale/angle caméra optionnels) en `key=value` dans le chunk projet de notre PCM_source. Un projet rouvert rebind chaque item sans action manuelle. Forward-compat : `LoadState` ignore les clés inconnues.
  *Gate (AR19) : déposer 2-3 items → save → fermer/rouvrir le projet → chaque item retrouve son fichier et joue ; aucun item vide/cassé.*

- **Story 6.2 — Persister l'état panneau/viewport au save/load** (FR32 reformulé)
  Position/état de dock du viewport restaurés via l'état **dock/screenset natif Reaper** (DockWindowAddEx s'intègre déjà au docker) ou notre ext-state — **pas** ReaImGui (hors MVP).
  *Gate : docker le panneau à un endroit → save/reopen → il revient au même endroit.*

- **Story 6.3 — Portabilité multi-machines par média natif Reaper** (FR46 reformulé)
  **Vérifier d'abord** : « Save project as… → copy media into project directory » copie-t-il le fichier d'animation de nos items localement, et le relink natif retrouve-t-il un média déplacé (via `GetFileName`/`SetFileName`) ? Si oui → **aucun code**, on documente que la portabilité est native. Si non → faire participer notre source au mécanisme natif (pas de remap maison).
  *Gate : projet sauvé avec « copy media » → rouvert sur une 2ᵉ machine (ou dossier déplacé) → items résolus sans ré-import forcé.*

### Epic 8 (NOUVEAU, post-release) — « Robustness, reload & format-coverage »

Récupère ce qui sort d'Epic 6, déféré après le ship :
- **8.1** Reload manuel (FR36) — bouton « Reload » qui repique les changements disque.
- **8.2** Dégradation gracieuse + isolation par-item + diagnostics de chargement (FR34, FR35, FR37) — pas de crash hôte sur fichier malformé, un item KO n'affecte pas les autres.
- **8.3** Validation FBX (cible 60 % corpus Demute) + textures FBX embarquées (FR3, FR19) + Collada best-effort (FR47).

*(Note : un filet minimal anti-crash existe déjà de fait — factories no-throw, source 0-canal silencieuse. Le durcissement formel et le reporting sont ce qui est déféré.)*

### Epic 5 (postponé, post-release) — « Browser in-Reaper »

Inchangé sur le fond (FR42–FR45 + FR30/AR3 ReaImGui), juste re-séquencé après Epic 7. Introduira la 1ʳᵉ dépendance ReaImGui quand on le reprendra.

---

## Section 4 — Detailed Change Proposals

### 4.1 — `epics.md`
- **Epic 5** : ajouter en tête de section un bandeau *« STATUS: POSTPONÉ — post-release (après Epic 7). Le système d'items d'Epic 4 couvre le placement ; le browser devient confort post-MVP. — Antho 2026-06-27 »*.
- **Epic 6** : remplacer titre + paragraphe + liste FR par la version « Save and recall Reaper sessions » ci-dessus ; remplacer les stories 6.1–6.5 actuelles par 6.1–6.3.
- **Epic 8** : nouvelle section (Robustness, reload & format-coverage), stories 8.1–8.3, marquée post-release.

### 4.2 — `prd.md`
- **FR32** → réécrire : *« …dock position via Reaper's native docker/screenset state (the viewport is a docked GL window; ReaImGui is post-MVP). »*
- **FR46** → réécrire : *« …resolve each item's animation path by relying on Reaper's native media handling (copy-into-project + relink via the PCM_source filename), verified in-Reaper; no custom relative-path/remap engine. Final mechanism: Story 6.3. »*
- Annoter FR3/FR19/FR34/FR35/FR36/FR37/FR47 « (Epic 8, post-release) » et FR42–FR45 « (Epic 5, postponé) ».

### 4.3 — `architecture.md` (Spec Change Log, nouvelle entrée 2026-06-27)
- D9 : dock position → **natif Reaper** (lignes 46, 77, 323-327, 824, 1003).
- FR46 / Phase 4 (489-491) : portabilité **native**, pas de remap maison ; Phase 4 = save/recall.

### 4.4 — `sprint-status.yaml` (à l'étape handoff, après approbation)
- Epic 5 : commentaire `# POSTPONÉ post-release`.
- Epic 6 : remplacer `6-1…6-5` par `6-1-persist-and-restore-per-item-state`, `6-2-persist-panel-viewport-state`, `6-3-cross-machine-portability-via-native-reaper-media`.
- Epic 8 : ajouter `epic-8: backlog` + `8-1…8-3` + retrospective optional.

---

## Section 5 — Implementation Handoff

**Scope : Moderate** → réorganisation de backlog (PO/DEV), pas de replan stratégique (le PRD *produit* est intact, seul le séquençage change).

- **Dev (Claude)** applique, après approbation : edits `epics.md`, `prd.md`, entrée Spec Change Log `architecture.md`, et `sprint-status.yaml`.
- **Première story exécutable ensuite : Story 6.1** (créer la story spec via `bmad-create-story`, puis dev). Elle débloque le cœur « save/recall ».
- **Action de vérif explicite (Story 6.3, peut être faite par Antho en amont)** : tester en-Reaper si « copy media into project » embarque nos items → détermine si 6.3 est *zéro code* ou *implémentation native*.

**Critère de succès du changement :** la route restante est sans ambiguïté = **Epic 6 (save/recall) → Epic 7 (ship)** ; browser + robustesse explicitement post-release ; FR32/FR46 alignés sur la réalité (GL docké + média natif Reaper).

---

## Checklist de navigation — synthèse

- **§1 Trigger** : [x] Epic 4 (item system) déplace la valeur ; browser secondaire ; persistance = vrai trou (SaveState/LoadState stubs).
- **§2 Epic impact** : [x] Epic 5 postponé, Epic 6 recentré, Epic 8 créé, Epic 7 intact, Epics 1–4 intouchés.
- **§3 Artefacts** : [x] PRD (FR32/FR46 reformulées + re-séquençage), Architecture (Spec Change Log D9 + FR46/Phase 4), Epics, sprint-status. UX : [N/A] (pas de browser UI dans le scope recentré).
- **§4 Path forward** : [x] Direct Adjustment + re-séquençage (Low/Low). Rollback [non-viable], MVP-review [partiel : report ≠ suppression].
- **§5 Handoff** : [x] Moderate, PO/DEV, première story = 6.1.
