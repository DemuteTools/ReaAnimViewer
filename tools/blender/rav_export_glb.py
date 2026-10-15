# SPDX-License-Identifier: MIT
#
# Blender -> ReaAnimViewer: exports the selected character as .glb files RAV reads cleanly.
#
# How to use (Blender 3.6 to 4.x):
#   1. Select the armature (or one of its meshes) in Object mode.
#   2. Open this file in the Text Editor (Scripting tab) and press Run Script (Alt+P).
#   -> one .glb per animation, written in a "RAV_export" folder next to the .blend file.
#
# What it sets for RAV:
#   - mesh + armature together, textures embedded in the .glb, +Y up;
#   - ONE animation per file (RAV only plays the first animation of a file);
#   - no Draco compression, no shape keys (RAV reads neither), 4 bone influences per vertex.
#   - a prop held by a bone (Child Of / Copy Transforms constraint, or bone parent) is exported
#     skinned to that bone, so it follows the hand in RAV (RAV only animates skinned meshes).

import os
import bpy

# --- Settings ----------------------------------------------------------------------------
ALL_ACTIONS = True   # True: one .glb per action of the armature. False: the active action only.
OUT_DIR = ""         # empty = "RAV_export" folder next to the .blend file
# -----------------------------------------------------------------------------------------


def find_armature(obj):
    if obj is None:
        return None
    if obj.type == 'ARMATURE':
        return obj
    if obj.parent and obj.parent.type == 'ARMATURE':
        return obj.parent
    for mod in getattr(obj, "modifiers", []):
        if mod.type == 'ARMATURE' and mod.object:
            return mod.object
    return None


def meshes_of(arm):
    out = []
    for obj in bpy.context.scene.objects:
        if obj.type != 'MESH':
            continue
        if obj.parent == arm or any(m.type == 'ARMATURE' and m.object == arm for m in obj.modifiers):
            out.append(obj)
    return out


def followed_bone(obj, arm):
    # The bone of `arm` a prop (sword, shield...) follows without being skinned: a bone
    # parent, or a Child Of / Copy Transforms / Armature constraint on that bone.
    if obj.parent == arm and obj.parent_type == 'BONE' and obj.parent_bone in arm.data.bones:
        return obj.parent_bone
    for c in obj.constraints:
        if c.mute or c.influence <= 0.0:
            continue
        if c.type in ('CHILD_OF', 'COPY_TRANSFORMS') and c.target == arm and c.subtarget in arm.data.bones:
            return c.subtarget
        if c.type == 'ARMATURE':
            for t in c.targets:
                if t.target == arm and t.subtarget in arm.data.bones:
                    return t.subtarget
    return None


def make_skinned_props(arm, skinned):
    # RAV only animates SKINNED meshes: a prop held by constraint or bone parent would stay
    # frozen. Export a temporary copy of it, skinned 100% to the bone it follows, placed
    # where it sits in the rest pose. The copies are deleted after the export.
    props = []
    for obj in bpy.context.view_layer.objects:
        if obj.type != 'MESH' or obj in skinned or not obj.visible_get():
            continue
        bone = followed_bone(obj, arm)
        if bone:
            props.append((obj, bone))
    if not props:
        return []

    old_pose = arm.data.pose_position
    arm.data.pose_position = 'REST'
    bpy.context.view_layer.update()
    rest = [(obj, bone, obj.matrix_world.copy()) for obj, bone in props]
    arm.data.pose_position = old_pose
    bpy.context.view_layer.update()

    temps = []
    for obj, bone, mw in rest:
        tmp = obj.copy()
        tmp.data = obj.data.copy()
        tmp.animation_data_clear()
        tmp.constraints.clear()
        bpy.context.scene.collection.objects.link(tmp)
        tmp.parent = arm
        tmp.parent_type = 'OBJECT'
        tmp.matrix_world = mw
        vg = tmp.vertex_groups.new(name=bone)
        vg.add(range(len(tmp.data.vertices)), 1.0, 'REPLACE')
        mod = tmp.modifiers.new("RAV_Armature", 'ARMATURE')
        mod.object = arm
        temps.append(tmp)
        print("RAV export: '%s' attached to bone '%s'" % (obj.name, bone))
    return temps


def actions_for(arm):
    # Actions animating this armature's bones (their F-curves name a bone of it).
    bones = {b.name for b in arm.data.bones}
    found = []
    for act in bpy.data.actions:
        try:
            curves = act.fcurves
        except AttributeError:          # Blender 5 layered actions: keep them all
            found.append(act)
            continue
        for fc in curves:
            if fc.data_path.startswith('pose.bones["'):
                if fc.data_path.split('"')[1] in bones:
                    found.append(act)
                    break
    return found


def set_action(arm, act):
    ad = arm.animation_data or arm.animation_data_create()
    ad.action = act
    # Blender 4.4+: an action needs a slot to drive the object.
    if act is not None and hasattr(ad, "action_slot") and ad.action_slot is None:
        slots = [s for s in getattr(act, "slots", []) if getattr(s, "target_id_type", "OBJECT") == 'OBJECT']
        if slots:
            ad.action_slot = slots[0]


def gltf_kwargs(path):
    wanted = dict(
        filepath=path,
        export_format='GLB',
        use_selection=True,
        export_yup=True,
        export_apply=True,                      # apply modifiers (except Armature)
        export_texcoords=True,
        export_normals=True,
        export_materials='EXPORT',
        export_image_format='AUTO',             # textures embedded in the .glb
        export_draco_mesh_compression_enable=False,
        export_skins=True,
        export_all_influences=False,            # 4 influences per vertex
        export_morph=False,                     # RAV does not play shape keys
        export_animations=True,
        export_animation_mode='ACTIVE_ACTIONS', # Blender 4.x: the active action only
        export_nla_strips=False,                # Blender 3.x equivalent
        export_force_sampling=True,
        export_frame_range=False,               # the action's own range
        export_cameras=False,
        export_lights=False,
    )
    # Keep only the options this Blender's exporter knows.
    known = {p.identifier for p in bpy.ops.export_scene.gltf.get_rna_type().properties}
    return {k: v for k, v in wanted.items() if k in known}


def safe_name(name):
    return "".join(c if c.isalnum() or c in "-_ ." else "_" for c in name).strip() or "anim"


def main():
    if bpy.context.mode != 'OBJECT':
        bpy.ops.object.mode_set(mode='OBJECT')

    arm = find_armature(bpy.context.active_object)
    if arm is None:
        raise RuntimeError("RAV export: select the armature or one of its meshes first.")
    meshes = meshes_of(arm)
    if not meshes:
        raise RuntimeError("RAV export: no mesh is skinned to '%s'." % arm.name)

    out_dir = OUT_DIR or os.path.join(bpy.path.abspath("//") or os.path.expanduser("~"), "RAV_export")
    os.makedirs(out_dir, exist_ok=True)

    ad = arm.animation_data
    old_action = ad.action if ad else None
    old_selected = list(bpy.context.selected_objects)
    old_active = bpy.context.view_layer.objects.active

    actions = actions_for(arm) if ALL_ACTIONS else [old_action]
    actions = [a for a in actions if a is not None]
    if not actions:
        raise RuntimeError("RAV export: '%s' has no animation (no action)." % arm.name)

    written = []
    temps = []
    try:
        temps = make_skinned_props(arm, meshes)

        # Select exactly the armature + its meshes + the props it holds.
        bpy.ops.object.select_all(action='DESELECT')
        for obj in [arm] + meshes + temps:
            obj.select_set(True)
        bpy.context.view_layer.objects.active = arm


        for act in actions:
            set_action(arm, act)
            path = os.path.join(out_dir, safe_name(act.name) + ".glb")
            bpy.ops.export_scene.gltf(**gltf_kwargs(path))
            written.append(path)
    finally:
        for tmp in temps:
            data = tmp.data
            bpy.data.objects.remove(tmp, do_unlink=True)
            if data.users == 0:
                bpy.data.meshes.remove(data)
        set_action(arm, old_action)
        bpy.ops.object.select_all(action='DESELECT')
        for obj in old_selected:
            obj.select_set(True)
        bpy.context.view_layer.objects.active = old_active

    print("RAV export: %d file(s) in %s" % (len(written), out_dir))
    for p in written:
        print("  ", os.path.basename(p))
    bpy.context.window_manager.popup_menu(
        lambda self, ctx: self.layout.label(text="%d .glb written in %s" % (len(written), out_dir)),
        title="RAV export", icon='CHECKMARK')


main()
