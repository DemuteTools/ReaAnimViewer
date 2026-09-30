# Exporting animations from Unreal Engine 5 to the viewer (step-by-step)

> ⚠️ **These steps are for Unreal Engine 5** (written and tested on **UE 5.8**). On another major Unreal version the
> labels and menu locations may differ — but the logic (Skeletal Mesh in a level + `.glb`) stays the same.
>
> **Goal:** produce a single **`.glb`** file containing **the mesh + its textures + one animation**, in a format the
> viewer loads with no error, with the texture visible and the animation playing.
>
> **Plugin used:** **glTF Exporter v1.3.1** (Epic Games).

---

## 1. Why `.glb` (not FBX)

| Format | Geometry | Skeleton + anim | Textures | Verdict |
| ------ | :------: | :-------------: | :------: | ------- |
| **FBX** | ✅ | ✅ | ❌ (referenced externally, often lost) | Causes "no texture" + parsing errors |
| **glTF `.gltf`** | ✅ | ✅ | ⚠️ (separate `.png` files + a `.bin`) | Works, but scattered across many files |
| **glTF binary `.glb`** | ✅ | ✅ | ✅ **embedded in the file** | ✅ **The right choice** |

`.glb` packs **everything into one file**, textures included. It also handles the axis conversion (Unreal is Z-up,
glTF is Y-up), so the character shows upright.

**Rule of thumb: always export as `GL Transmission Format (Binary) (*.glb)`.**

---

## 2. Prerequisite: enable the glTF Exporter plugin

Since Unreal 5.1 the glTF Exporter ships with the engine but is **not always enabled**. If `.glb` doesn't appear in
the export list below, enable it:

1. **Edit → Plugins**
2. Search box: type `gltf`
3. Tick **"glTF Exporter"** (author: Epic Games, category *Importers/Exporters*)
4. Click **"Restart Now"** to restart the editor

> If it's already ticked (as in our case, v1.3.1), there's nothing to do.

---

## 3. The key concept (or you'll go in circles)

Unreal has **three different things** that look alike:

| Asset | Content Browser label | Typical prefix | What it is |
| ----- | --------------------- | -------------- | ---------- |
| **Skeleton** | "Skeleton" | `SK_` | Just the bone hierarchy, **no mesh** → exports only T3D, **no glb** |
| **Skeletal Mesh** | "Skeletal Mesh" | `SKM_` | The **body** (mesh + skin) → exports to `.glb`, **but in rest pose, without the animation** |
| **Animation Sequence** | "Animation Sequence" | (animated thumbnail) | **One** animation → right-click **Export only offers FBX**, no glb |

**The consequence to remember absolutely:**

- Exporting the **Animation Sequence** directly → FBX only. ❌
- Exporting the **Skeletal Mesh** directly → a `.glb`, but the character is in **A-pose, no animation**. ❌
- **To get mesh + animation in one `.glb`, you must place the Skeletal Mesh in a level, assign it the animation, and
  export the selection.** ✅ ← **this is the method below.**

---

## 4. Full method: mesh + animation → a single `.glb`

### Step 1: Find the right asset (the Skeletal Mesh)

In the **Content Browser**, find the asset whose bottom label says **"Skeletal Mesh"** (often `SKM_...`).

⚠️ **Do not pick** the "Skeleton" (`SK_...`, blue): it has no mesh and only exports T3D.
Real example: `SK_Mannequin` = Skeleton (wrong), `SKM_Manny_Simple` = Skeletal Mesh (right).

> Tip: if you start from an animation, double-click it to open the Animation editor; the character shown **is** that
> Skeletal Mesh. The Skeletal Mesh usually lives in the same or a neighboring folder.

### Step 2: Create an empty level

**File → New Level… → Empty Level** (an empty level is enough; any open level works too).

### Step 3: Place the mesh in the scene

**Drag the Skeletal Mesh** from the Content Browser **into the 3D viewport**. This creates an **actor**
(a "Skeletal Mesh Actor") in the level.

### Step 4: Reset the actor to the origin (important)

Select the actor, then in the **Details** panel (right) → **Transform → Location** → set **`0, 0, 0`**.

> **Why:** if the actor sits elsewhere in the world, its position is baked into the export and can skew/distort the
> display in the viewer. Placing it at the origin guarantees a clean render.
> *(A viewer-side fix is in progress — but keep this habit, it's safer.)*

### Step 5: Assign the animation to the actor

With the actor still selected, open the **Details** panel (docked on the right by default — if it's missing:
**Window → Details**). Near the top it has category tabs (**General · Actor · Animation · LOD · …**) and a search box.

1. Click the **Animation** tab (or type `Animation Mode` in the Details search box).
2. **Animation Mode** → change it from the default **`Use Animation Blueprint`** to **`Use Animation Asset`**.
   *(This is the key switch — in Blueprint mode the exporter has no single animation to bake.)*
3. A field **`Anim to Play`** appears (replacing "Anim Class") → choose **your animation** (e.g. `MM_Death_Back_01`).

→ The character should take the pose / play the animation in the viewport. If nothing moves, double-check the two
settings above.

> **UE5 note:** these are the properties of the **Skeletal Mesh Component**. If the Animation section looks empty,
> make sure the placed actor (not a camera/light/the level) is selected in the **Outliner**.

### Step 6: Export the selection

With the actor **still selected**: menu **File → Export Selected…**

### Step 7: Choose the `.glb` format

In the save dialog, open the **"Save as type"** dropdown and choose:

> **`GL Transmission Format (Binary) (*.glb)`**

⚠️ **Not** `GL Transmission Format (*.gltf)` (that one splits into `.gltf` + `.bin` + many `.png`).
⚠️ **Not** `FBX`.

Name the file, pick a location, **Save**.

### Step 8: Set the glTF options

A **"glTF (Binary) Export Options"** window opens. The settings that matter:

- **Mesh → `Export Vertex Skin Weights`**: ✅ **checked** (mandatory — without it, no deformation)
- **Animation → `Export Animation Sequences`**: ✅ **checked** (embeds the animation)
- **General → `Export Uniform Scale`**: leave **`0.01`** (Unreal cm → meters conversion)
- The rest can stay at defaults.

Click **`Export`**.

### Step 9: Ignore the lightmap warnings

You may see warnings like:
`Material ... is baked using mesh data ... lightmap UV (channel 0) are overlapping ... may produce incorrect results`

**This is cosmetic and harmless for you.** Unreal "bakes" its node-graph materials into flat textures for glTF; the
warning is about lightmaps, which the viewer doesn't use. The `.glb` is created fine.

### Step 10: Load it in the viewer

Open the `.glb` in the viewer. You should get: **visible texture**, **upright character**, **animation playing**.

---

## 5. Quick variant: mesh only (no animation)

If you just want to see the **textured model** (rest pose, no animation): right-click the **Skeletal Mesh** →
**Asset Actions → Export…** → `.glb`. No level needed. (But there will be **no** animation — that's expected.)

---

## 6. Troubleshooting (symptom → cause → fix)

| Symptom in Reaper / on export | Cause | Fix |
| ----------------------------- | ----- | --- |
| **Parsing error** on load | ASCII FBX or a recent FBX version assimp can't read | Export as **`.glb`** (§4). Last-resort FBX: **binary**, **FBX 2013** compatibility. |
| **No texture** | FBX doesn't embed textures | Export as **`.glb`** (textures embedded). |
| **Character lying down / wrong orientation** | Unreal Z-up rendered "as authored" | `.glb` converts to Y-up; otherwise reframe with **Reset Camera** / the **ViewCube**. |
| **Character in A-pose, no animation** | You exported the **Skeletal Mesh alone** | Use the **level + Anim to Play** method (§4). |
| **Dropdown only offers FBX** | You selected a **Skeleton** (`SK_`), or the plugin is off | Select the **Skeletal Mesh** (`SKM_`, §3); enable the plugin (§2). |
| **Many `.gltf` + `.bin` + `.png` files** | You chose `.gltf` (text) instead of `.glb` | Re-export as **`GL Transmission Format (Binary) (*.glb)`** (§7). |
| **Explosion / fan of shards** | Actor not placed at the origin in the level | Reset **Location to `0,0,0`** (§4, step 4). |
| **Only the torso moves / shards at the shoulders** | Viewer-side bug (animated non-weighted bones dropped) | Fixed by story **3.4**; meanwhile nothing to change on export — your file is fine. |

---

## 7. Cheat sheet (short version)

1. **Skeletal Mesh** (`SKM_`), not the Skeleton (`SK_`).
2. **New Level → Empty**, drag the mesh in.
3. Actor → **Location `0,0,0`**.
4. Details → **Animation Mode = Use Animation Asset**, **Anim to Play = your anim**.
5. **File → Export Selected → `GL Transmission Format (Binary) (*.glb)`**.
6. Options: **Export Vertex Skin Weights ✅**, **Export Animation Sequences ✅**, Scale `0.01`.
7. Ignore the lightmap warnings → **Export**.
8. Load the `.glb` in the viewer.
