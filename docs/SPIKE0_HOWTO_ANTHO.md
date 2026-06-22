# Spike 0 — Guide pas-à-pas pour Antho (zéro expérience de compilation)

But : transformer le code du spike en une DLL, l'installer dans Reaper, ouvrir le
viewer, et lire les fps. Tu n'as **jamais besoin d'écrire de code** — juste suivre.

Le dossier du projet est déjà sur ton disque Windows :
**`D:\Git\Scripts\Reaper\FBXAnimationViewer`**
(les fichiers du spike y sont déjà, tu n'as rien à télécharger du projet lui-même).

⏱️ Compter ~1 h la première fois (surtout l'installation de Visual Studio).

---

## Étape 1 — Installer les outils (une seule fois)

### 1a. Visual Studio 2022 (le compilateur C++) — gratuit

1. Va sur <https://visualstudio.microsoft.com/fr/downloads/>.
2. Sous **« Visual Studio Community 2022 »**, clique **Télécharger gratuitement**.
3. Lance le fichier téléchargé. Une fenêtre « Visual Studio Installer » s'ouvre.
4. Dans l'onglet **« Charges de travail »**, coche **« Développement Desktop en C++ »**
   (case avec une icône C++). Ne coche rien d'autre.
5. Clique **Installer** en bas à droite. ⏳ Ça télécharge plusieurs Go (10–20 min).
6. Quand c'est fini, tu peux fermer l'installeur. Pas besoin de redémarrer le PC.

### 1b. CMake (l'outil qui prépare le build) — gratuit

1. Va sur <https://cmake.org/download/>.
2. Section **« Binary distributions »** → télécharge **« Windows x64 Installer »**
   (fichier `.msi`).
3. Lance-le. **IMPORTANT** : à l'écran qui propose le PATH, choisis
   **« Add CMake to the system PATH for all users »** (sinon les commandes plus bas
   ne marcheront pas). Puis Next → Install → Finish.

### 1c. ReaImGui dans Reaper (nécessaire pour faire tourner le viewer)

1. Ouvre Reaper.
2. Menu **Extensions → ReaPack → Browse packages…**
   *(Si tu n'as pas ReaPack du tout : installe-le d'abord depuis <https://reapack.com/>,
   puis redémarre Reaper. Mais tu l'as sûrement déjà.)*
3. Dans la barre de recherche, tape **`ReaImGui`**.
4. Clic droit sur **« ReaImGui: ReaScript binding for Dear ImGui »** (auteur cfillion)
   → **Install**.
5. Clique **Apply** (en bas). Laisse-le télécharger.
6. **Note la version installée** (ex. `0.10.0.5` ou plus récent) — affichée dans la
   ligne ReaPack. Tu en auras besoin à l'étape 1d.
7. **Ferme et rouvre Reaper.**

### 1d. Le fichier d'en-tête ReaImGui (un fichier que le code a besoin de lire)

Ce fichier est généré par ReaImGui, il n'est pas dans le projet. Récupère-le :

1. Va sur <https://github.com/cfillion/reaimgui/releases>.
2. Trouve la version **correspondant à celle que ReaPack a installée** (étape 1c-6).
   Déplie sa section **« Assets »**.
3. Télécharge le fichier nommé **`reaper_imgui_functions.h`**.
4. Copie-le dans le dossier :
   **`D:\Git\Scripts\Reaper\FBXAnimationViewer\extern\reaimgui\include\`**
   (le dossier existe déjà ; il contient un fichier `README_DROP_HEADER_HERE.md` —
   c'est le bon endroit).

> Alternative si tu ne trouves pas l'asset : dans Reaper, **Actions → Show action
> list…**, cherche **« [developer] Write C++ API functions header »**, lance-la ;
> elle écrit `reaper_imgui_functions.h` dans ton dossier de ressources Reaper —
> copie-le ensuite dans le dossier ci-dessus.

---

## Étape 2 — Mettre un fichier d'animation de test

Le viewer va charger un modèle 3D animé (un `.glb` « skinné », c.-à-d. avec un
squelette).

1. Crée le dossier **`D:\fixtures\`** (clic droit → Nouveau → Dossier dans `D:\`).
2. Mets-y un `.glb` animé, renommé exactement **`skinned.glb`** → donc le chemin final
   doit être **`D:\fixtures\skinned.glb`**.

D'où prendre un fichier de test connu pour fonctionner :
- Un de tes propres exports Demute (glTF/GLB skinné), **ou**
- Un échantillon gratuit Khronos : <https://github.com/KhronosGroup/glTF-Sample-Assets>
  → dossier `Models` → par ex. **`Fox`** ou **`CesiumMan`** → sous-dossier `glTF-Binary`
  → télécharge le `.glb`. Renomme-le `skinned.glb` et mets-le dans `D:\fixtures\`.

> Si tu préfères garder ton fichier ailleurs / sous un autre nom, dis-moi le chemin
> exact et je l'inscris dans le code avant que tu compiles (tu m'évites de te faire
> manipuler des variables d'environnement).

---

## Étape 3 — Compiler (créer la DLL)

1. Menu Démarrer → tape **« x64 Native Tools Command Prompt for VS 2022 »** → ouvre-le.
   *(C'est une fenêtre noire de commandes, préparée pour le compilateur 64-bit.)*
2. Copie-colle cette commande (pour aller dans le dossier du projet) et Entrée :
   ```
   cd /d D:\Git\Scripts\Reaper\FBXAnimationViewer
   ```
3. Copie-colle celle-ci et Entrée (elle prépare le build et télécharge 2 librairies
   depuis internet — assimp et GLM ; ça prend 1–3 min) :
   ```
   cmake -B build -G "Visual Studio 17 2022" -A x64
   ```
   ✅ Attendu : ça se termine par **`Generating done`** sans ligne rouge `CMake Error`.
4. Copie-colle celle-ci et Entrée (la vraie compilation ; **assimp est gros, compte
   5–15 min**, plein de texte défile, c'est normal) :
   ```
   cmake --build build --config Release
   ```
   ✅ Attendu, tout à la fin :
   ```
   fbxav_spike.vcxproj -> D:\Git\...\build\Release\reaper_fbxav_spike.dll
   ```

> ❌ **Si une commande s'arrête sur une erreur** : sélectionne le texte de la
> **première** ligne contenant `error` (avec la souris, puis Entrée pour copier),
> et envoie-la-moi. C'est du code jamais encore passé par un vrai compilateur, donc
> il est probable qu'il faille 1–2 petites corrections au premier essai — c'est
> normal et c'est mon boulot de les faire. Ne te bloque pas dessus.

---

## Étape 4 — Installer la DLL dans Reaper

1. Dans la **même** fenêtre de commandes, copie-colle et Entrée :
   ```
   copy build\Release\reaper_fbxav_spike.dll "%APPDATA%\REAPER\UserPlugins\"
   ```
   ✅ Attendu : `1 fichier(s) copié(s)`.
2. **Ferme complètement Reaper, puis rouvre-le** (pour qu'il charge la nouvelle DLL).

---

## Étape 5 — Lancer le viewer et lire les fps

1. Dans Reaper : **Extensions → ReaScript console output** (ou **View → Show console**).
   Tu dois y voir une ligne :
   `[FBXAV-spike] loaded (Spike 0). Run the action to open the viewer.`
2. **Actions → Show action list…**, tape **`FBXAV`** dans le filtre.
   Double-clique la ligne **« FBXAV: Open Spike Viewer (Spike 0) »**.
3. ✅ Une fenêtre ReaImGui s'ouvre : le modèle 3D doit **tourner et bouger** (s'animer),
   avec en haut une ligne **`fps: …`**.
4. Pour le **docker** : attrape la barre de titre de cette fenêtre et glisse-la sur un
   bord de Reaper (haut/bas/gauche/droite) — elle doit s'ancrer comme les autres
   panneaux.
5. **Lis le chiffre de fps** une fois stabilisé.

### Ce qui peut clocher (et le sens)

| Symptôme | Ce que ça veut dire / quoi faire |
|---|---|
| Console : `ReaImGui unavailable…` | ReaImGui pas (bien) installé → refais l'étape 1c. |
| Console : `load failed D:/fixtures/skinned.glb…` | Le fichier de test n'est pas au bon endroit/nom → étape 2. |
| Panneau gris, pas de modèle | Idem : vérifie le chemin du `.glb` et la console. |
| Erreur mentionnant `#version 330` / shader | Note-le-moi : il faudra un petit ajustement du contexte GL (prévu, facile). |
| Reaper plante | Note ce que tu faisais + version de Reaper (`Help → About`), envoie-moi. |

---

## Étape 6 — Noter les résultats

Ouvre **`docs\SPIKE0_FINDINGS.md`** (dans le dossier du projet, avec un éditeur de
texte ou VS Code) et remplis les cases marquées ⏳ :
- le build a réussi ? (oui/erreurs)
- le rig est visible et s'anime ? (oui/non)
- **le chiffre de fps** + la taille du panneau + quel fichier de test
- ≥ 60 fps ? (oui/non)
- quoi que ce soit de bizarre visuellement (modèle penché, à l'envers, trop grand…)

Puis **envoie-moi** : le chiffre de fps, une capture d'écran, et le contenu de la
console Reaper. Avec ça je remplis le **verdict go/no-go** et on clôt le spike.

---

## Rappel important

C'est un **prototype jetable** : sa branche `spike/0-1-feasibility` ne sera **jamais
fusionnée** dans le code de production. Son seul but est de répondre « est-ce que ça
tient les 60 fps ? ». Une fois la réponse notée, on repart proprement sur la
production (Epic 1) — en réutilisant ce qu'on a appris ici.
