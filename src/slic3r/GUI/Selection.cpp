#include "libslic3r/libslic3r.h"
#include "Selection.hpp"

#include "3DScene.hpp"
#include "GLCanvas3D.hpp"
#include "GUI_App.hpp"
#include "GUI.hpp"
#include "GUI_ObjectManipulation.hpp"
#include "GUI_ObjectList.hpp"
#include "Gizmos/GLGizmoBase.hpp"
#include "Camera.hpp"
#include "Plater.hpp"
#include "slic3r/Utils/UndoRedo.hpp"

#include "libslic3r/LocalesUtils.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/BuildVolume.hpp"

#include <GL/glew.h>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/log/trivial.hpp>

static const Slic3r::ColorRGBA UNIFORM_SCALE_COLOR     = Slic3r::ColorRGBA::ORANGE();
static const Slic3r::ColorRGBA SOLID_PLANE_COLOR       = Slic3r::ColorRGBA::ORANGE();
static const Slic3r::ColorRGBA TRANSPARENT_PLANE_COLOR = { 0.8f, 0.8f, 0.8f, 0.5f };

namespace Slic3r {
namespace GUI {

Selection::VolumeCache::TransformCache::TransformCache()
    : position(Vec3d::Zero())
    , rotation(Vec3d::Zero())
    , scaling_factor(Vec3d::Ones())
    , mirror(Vec3d::Ones())
    , rotation_matrix(Transform3d::Identity())
    , scale_matrix(Transform3d::Identity())
    , mirror_matrix(Transform3d::Identity())
    , full_matrix(Transform3d::Identity())
{
}

Selection::VolumeCache::TransformCache::TransformCache(const Geometry::Transformation& transform)
    : position(transform.get_offset())
    , rotation(transform.get_rotation())
    , scaling_factor(transform.get_scaling_factor())
    , mirror(transform.get_mirror())
    , full_matrix(transform.get_matrix())
{
    rotation_matrix = Geometry::assemble_transform(Vec3d::Zero(), rotation);
    scale_matrix = Geometry::assemble_transform(Vec3d::Zero(), Vec3d::Zero(), scaling_factor);
    mirror_matrix = Geometry::assemble_transform(Vec3d::Zero(), Vec3d::Zero(), Vec3d::Ones(), mirror);
}

Selection::VolumeCache::VolumeCache(const Geometry::Transformation& volume_transform, const Geometry::Transformation& instance_transform)
    : m_volume(volume_transform)
    , m_instance(instance_transform)
{
}

bool Selection::Clipboard::is_sla_compliant() const
{
    if (m_mode == Selection::Volume)
        return false;

    for (const ModelObject* o : m_model->objects) {
        if (o->is_multiparts())
            return false;

        for (const ModelVolume* v : o->volumes) {
            if (v->is_modifier())
                return false;
        }
    }

    return true;
}

Selection::Clipboard::Clipboard()
{
    m_model.reset(new Model);
}

void Selection::Clipboard::reset()
{
    m_model->clear_objects();
}

bool Selection::Clipboard::is_empty() const
{
    return m_model->objects.empty();
}

ModelObject* Selection::Clipboard::add_object()
{
    return m_model->add_object();
}

ModelObject* Selection::Clipboard::get_object(unsigned int id)
{
    return (id < (unsigned int)m_model->objects.size()) ? m_model->objects[id] : nullptr;
}

const ModelObjectPtrs& Selection::Clipboard::get_objects() const
{
    return m_model->objects;
}

Selection::Selection()
    : m_volumes(nullptr)
    , m_model(nullptr)
    , m_enabled(false)
    , m_mode(Instance)
    , m_type(Empty)
    , m_valid(false)
    , m_scale_factor(1.0f)
{
    this->set_bounding_boxes_dirty();
}


void Selection::set_volumes(GLVolumePtrs* volumes)
{
    m_volumes = volumes;
    update_valid();
}

// Init shall be called from the OpenGL render function, so that the OpenGL context is initialized!
bool Selection::init()
{
    m_arrow.init_from(straight_arrow(10.0f, 5.0f, 5.0f, 10.0f, 1.0f));
    m_curved_arrow.init_from(circular_arrow(16, 10.0f, 5.0f, 10.0f, 5.0f, 1.0f));
#if ENABLE_RENDER_SELECTION_CENTER
    m_vbo_sphere.init_from(its_make_sphere(0.75, PI / 12.0));
#endif // ENABLE_RENDER_SELECTION_CENTER

    return true;
}

void Selection::set_model(Model* model)
{
    m_model = model;
    update_valid();
}

void Selection::add(unsigned int volume_idx, bool as_single_selection, bool check_for_already_contained)
{
    if (!m_valid || (unsigned int)m_volumes->size() <= volume_idx)
        return;

    const GLVolume* volume = (*m_volumes)[volume_idx];
    // wipe tower is already selected
    if (is_wipe_tower() && volume->is_wipe_tower)
        return;

    bool keep_instance_mode = (m_mode == Instance) && !as_single_selection;
    bool already_contained = check_for_already_contained && contains_volume(volume_idx);

    // resets the current list if needed
    bool needs_reset = as_single_selection && !already_contained;
    needs_reset |= volume->is_wipe_tower;
    needs_reset |= is_wipe_tower() && !volume->is_wipe_tower;
    needs_reset |= as_single_selection && !is_any_modifier() && volume->is_modifier;
    needs_reset |= is_any_modifier() && !volume->is_modifier;

    if (!already_contained || needs_reset) {
        wxGetApp().plater()->take_snapshot(_L("Selection-Add"), UndoRedo::SnapshotType::Selection);

        if (needs_reset)
            clear();

        if (!keep_instance_mode)
            m_mode = volume->is_modifier ? Volume : Instance;
    }
    else
      // keep current mode
      return;

    switch (m_mode)
    {
    case Volume:
    {
        if (volume->volume_idx() >= 0 && (is_empty() || volume->instance_idx() == get_instance_idx()))
            do_add_volume(volume_idx);

        break;
    }
    case Instance:
    {
        Plater::SuppressSnapshots suppress(wxGetApp().plater());
        add_instance(volume->object_idx(), volume->instance_idx(), as_single_selection);
        break;
    }
    }

    update_type();
    this->set_bounding_boxes_dirty();
}

void Selection::remove(unsigned int volume_idx)
{
    if (!m_valid || (unsigned int)m_volumes->size() <= volume_idx)
        return;

    if (!contains_volume(volume_idx))
        return;

    wxGetApp().plater()->take_snapshot(_L("Selection-Remove"), UndoRedo::SnapshotType::Selection);

    GLVolume* volume = (*m_volumes)[volume_idx];

    switch (m_mode)
    {
    case Volume:
    {
        do_remove_volume(volume_idx);
        break;
    }
    case Instance:
    {
        do_remove_instance(volume->object_idx(), volume->instance_idx());
        break;
    }
    }

    update_type();
    this->set_bounding_boxes_dirty();
}

void Selection::add_object(unsigned int object_idx, bool as_single_selection)
{
    if (!m_valid)
        return;

    std::vector<unsigned int> volume_idxs = get_volume_idxs_from_object(object_idx);
    if ((!as_single_selection && contains_all_volumes(volume_idxs)) ||
        (as_single_selection && matches(volume_idxs)))
        return;

    wxGetApp().plater()->take_snapshot(_L("Selection-Add Object"), UndoRedo::SnapshotType::Selection);

    // resets the current list if needed
    if (as_single_selection)
        clear();

    m_mode = Instance;

    do_add_volumes(volume_idxs);

    update_type();
    this->set_bounding_boxes_dirty();
}

void Selection::remove_object(unsigned int object_idx)
{
    if (!m_valid)
        return;

    wxGetApp().plater()->take_snapshot(_L("Selection-Remove Object"), UndoRedo::SnapshotType::Selection);

    do_remove_object(object_idx);

    update_type();
    this->set_bounding_boxes_dirty();
}

void Selection::add_instance(unsigned int object_idx, unsigned int instance_idx, bool as_single_selection)
{
    if (!m_valid)
        return;

    const std::vector<unsigned int> volume_idxs = get_volume_idxs_from_instance(object_idx, instance_idx);
    if ((!as_single_selection && contains_all_volumes(volume_idxs)) ||
        (as_single_selection && matches(volume_idxs)))
        return;

    wxGetApp().plater()->take_snapshot(_L("Selection-Add Instance"), UndoRedo::SnapshotType::Selection);

    // resets the current list if needed
    if (as_single_selection)
        clear();

    m_mode = Instance;

    do_add_volumes(volume_idxs);

    update_type();
    this->set_bounding_boxes_dirty();
}

void Selection::remove_instance(unsigned int object_idx, unsigned int instance_idx)
{
    if (!m_valid)
        return;

    wxGetApp().plater()->take_snapshot(_L("Selection-Remove Instance"), UndoRedo::SnapshotType::Selection);

    do_remove_instance(object_idx, instance_idx);

    update_type();
    this->set_bounding_boxes_dirty();
}

void Selection::add_volume(unsigned int object_idx, unsigned int volume_idx, int instance_idx, bool as_single_selection)
{
    if (!m_valid)
        return;

    std::vector<unsigned int> volume_idxs = get_volume_idxs_from_volume(object_idx, instance_idx, volume_idx);
    if ((!as_single_selection && contains_all_volumes(volume_idxs)) ||
        (as_single_selection && matches(volume_idxs)))
        return;

    // resets the current list if needed
    if (as_single_selection)
        clear();

    m_mode = Volume;

    do_add_volumes(volume_idxs);

    update_type();
    this->set_bounding_boxes_dirty();
}

void Selection::remove_volume(unsigned int object_idx, unsigned int volume_idx)
{
    if (!m_valid)
        return;

    for (unsigned int i = 0; i < (unsigned int)m_volumes->size(); ++i) {
        GLVolume* v = (*m_volumes)[i];
        if (v->object_idx() == (int)object_idx && v->volume_idx() == (int)volume_idx)
            do_remove_volume(i);
    }

    update_type();
    this->set_bounding_boxes_dirty();
}

void Selection::add_volumes(EMode mode, const std::vector<unsigned int>& volume_idxs, bool as_single_selection)
{
    if (!m_valid)
        return;

    if ((!as_single_selection && contains_all_volumes(volume_idxs)) ||
        (as_single_selection && matches(volume_idxs)))
        return;

    // resets the current list if needed
    if (as_single_selection)
        clear();

    m_mode = mode;
    for (unsigned int i : volume_idxs) {
        if (i < (unsigned int)m_volumes->size())
            do_add_volume(i);
    }

    update_type();
    this->set_bounding_boxes_dirty();
}

void Selection::remove_volumes(EMode mode, const std::vector<unsigned int>& volume_idxs)
{
    if (!m_valid)
        return;

    m_mode = mode;
    for (unsigned int i : volume_idxs) {
        if (i < (unsigned int)m_volumes->size())
            do_remove_volume(i);
    }

    update_type();
    this->set_bounding_boxes_dirty();
}

void Selection::add_all()
{
    if (!m_valid)
        return;

    unsigned int count = 0;
    for (unsigned int i = 0; i < (unsigned int)m_volumes->size(); ++i) {
        if (!(*m_volumes)[i]->is_wipe_tower)
            ++count;
    }

    if ((unsigned int)m_list.size() == count)
        return;
    
    wxGetApp().plater()->take_snapshot(_(L("Selection-Add All")), UndoRedo::SnapshotType::Selection);

    m_mode = Instance;
    clear();

    for (unsigned int i = 0; i < (unsigned int)m_volumes->size(); ++i) {
        if (!(*m_volumes)[i]->is_wipe_tower)
            do_add_volume(i);
    }

    update_type();
    this->set_bounding_boxes_dirty();
}

void Selection::remove_all()
{
    if (!m_valid)
        return;

    if (is_empty())
        return;
  
// Not taking the snapshot with non-empty Redo stack will likely be more confusing than losing the Redo stack.
// Let's wait for user feedback.
//    if (!wxGetApp().plater()->can_redo())
        wxGetApp().plater()->take_snapshot(_L("Selection-Remove All"), UndoRedo::SnapshotType::Selection);

    m_mode = Instance;
    clear();
}

void Selection::set_deserialized(EMode mode, const std::vector<std::pair<size_t, size_t>> &volumes_and_instances)
{
    if (! m_valid)
        return;

    m_mode = mode;
    for (unsigned int i : m_list)
        (*m_volumes)[i]->selected = false;
    m_list.clear();
    for (unsigned int i = 0; i < (unsigned int)m_volumes->size(); ++ i)
		if (std::binary_search(volumes_and_instances.begin(), volumes_and_instances.end(), (*m_volumes)[i]->geometry_id))
			do_add_volume(i);
    update_type();
    set_bounding_boxes_dirty();
}

void Selection::clear()
{
    if (!m_valid)
        return;

    if (m_list.empty())
        return;

    // ensure that the volumes get the proper color before next call to render (expecially needed for transparent volumes)
    for (unsigned int i : m_list) {
        GLVolume& volume = *(*m_volumes)[i];
        volume.selected = false;
        volume.set_render_color(volume.color.is_transparent());
    }

    m_list.clear();

    update_type();
    set_bounding_boxes_dirty();

    // this happens while the application is closing
    if (wxGetApp().obj_manipul() == nullptr)
        return;

    // resets the cache in the sidebar
    wxGetApp().obj_manipul()->reset_cache();

    // #et_FIXME fake KillFocus from sidebar
    wxGetApp().plater()->canvas3D()->handle_sidebar_focus_event("", false);
}

// Update the selection based on the new instance IDs.
void Selection::instances_changed(const std::vector<size_t> &instance_ids_selected)
{
    assert(m_valid);
    assert(m_mode == Instance);
    m_list.clear();
    for (unsigned int volume_idx = 0; volume_idx < (unsigned int)m_volumes->size(); ++ volume_idx) {
        const GLVolume *volume = (*m_volumes)[volume_idx];
        auto it = std::lower_bound(instance_ids_selected.begin(), instance_ids_selected.end(), volume->geometry_id.second);
		if (it != instance_ids_selected.end() && *it == volume->geometry_id.second)
            this->do_add_volume(volume_idx);
    }
    update_type();
    this->set_bounding_boxes_dirty();
}

// Update the selection based on the map from old indices to new indices after m_volumes changed.
// If the current selection is by instance, this call may select newly added volumes, if they belong to already selected instances.
void Selection::volumes_changed(const std::vector<size_t> &map_volume_old_to_new)
{
    assert(m_valid);
    assert(m_mode == Volume);
    IndicesList list_new;
    for (unsigned int idx : m_list)
        if (map_volume_old_to_new[idx] != size_t(-1)) {
            unsigned int new_idx = (unsigned int)map_volume_old_to_new[idx];
            (*m_volumes)[new_idx]->selected = true;
            list_new.insert(new_idx);
        }
    m_list = std::move(list_new);
    update_type();
    this->set_bounding_boxes_dirty();
}

bool Selection::is_single_full_instance() const
{
    if (m_type == SingleFullInstance)
        return true;

    if (m_type == SingleFullObject)
        return get_instance_idx() != -1;

    if (m_list.empty() || m_volumes->empty())
        return false;

    int object_idx = m_valid ? get_object_idx() : -1;
    if (object_idx < 0 || (int)m_model->objects.size() <= object_idx)
        return false;

    int instance_idx = (*m_volumes)[*m_list.begin()]->instance_idx();

    std::set<int> volumes_idxs;
    for (unsigned int i : m_list) {
        const GLVolume* v = (*m_volumes)[i];
        if (object_idx != v->object_idx() || instance_idx != v->instance_idx())
            return false;

        int volume_idx = v->volume_idx();
        if (volume_idx >= 0)
            volumes_idxs.insert(volume_idx);
    }

    return m_model->objects[object_idx]->volumes.size() == volumes_idxs.size();
}

bool Selection::is_from_single_object() const
{
    const int idx = get_object_idx();
#if ENABLE_WIPETOWER_OBJECTID_1000_REMOVAL
    return 0 <= idx && idx < int(m_model->objects.size());
#else
    return 0 <= idx && idx < 1000;
#endif // ENABLE_WIPETOWER_OBJECTID_1000_REMOVAL
}

bool Selection::is_sla_compliant() const
{
    if (m_mode == Volume)
        return false;

    for (unsigned int i : m_list) {
        if ((*m_volumes)[i]->is_modifier)
            return false;
    }

    return true;
}

bool Selection::contains_all_volumes(const std::vector<unsigned int>& volume_idxs) const
{
    for (unsigned int i : volume_idxs) {
        if (m_list.find(i) == m_list.end())
            return false;
    }

    return true;
}

bool Selection::contains_any_volume(const std::vector<unsigned int>& volume_idxs) const
{
    for (unsigned int i : volume_idxs) {
        if (m_list.find(i) != m_list.end())
            return true;
    }

    return false;
}

bool Selection::matches(const std::vector<unsigned int>& volume_idxs) const
{
    unsigned int count = 0;

    for (unsigned int i : volume_idxs) {
        if (m_list.find(i) != m_list.end())
            ++count;
        else
            return false;
    }

    return count == (unsigned int)m_list.size();
}

bool Selection::requires_uniform_scale() const
{
#if ENABLE_WORLD_COORDINATE
    if (is_single_modifier() || is_single_volume())
        return !Geometry::is_rotation_ninety_degrees(Geometry::Transformation(get_volume(*m_list.begin())->world_matrix()).get_rotation());
    else if (is_single_full_instance() && wxGetApp().obj_manipul()->get_world_coordinates())
        return !Geometry::is_rotation_ninety_degrees(get_volume(*m_list.begin())->get_instance_rotation());

    return false;
#else
    if (is_single_full_instance() || is_single_modifier() || is_single_volume())
        return false;

    return true;
#endif // ENABLE_WORLD_COORDINATE
}

int Selection::get_object_idx() const
{
    return (m_cache.content.size() == 1) ? m_cache.content.begin()->first : -1;
}

int Selection::get_instance_idx() const
{
    if (m_cache.content.size() == 1) {
        const InstanceIdxsList& idxs = m_cache.content.begin()->second;
        if (idxs.size() == 1)
            return *idxs.begin();
    }

    return -1;
}

const Selection::InstanceIdxsList& Selection::get_instance_idxs() const
{
    assert(m_cache.content.size() == 1);
    return m_cache.content.begin()->second;
}

const GLVolume* Selection::get_volume(unsigned int volume_idx) const
{
    return (m_valid && (volume_idx < (unsigned int)m_volumes->size())) ? (*m_volumes)[volume_idx] : nullptr;
}

const BoundingBoxf3& Selection::get_bounding_box() const
{
    if (!m_bounding_box.has_value()) {
        std::optional<BoundingBoxf3>* bbox = const_cast<std::optional<BoundingBoxf3>*>(&m_bounding_box);
        *bbox = BoundingBoxf3();
        if (m_valid) {
            for (unsigned int i : m_list) {
                (*bbox)->merge((*m_volumes)[i]->transformed_convex_hull_bounding_box());
            }
        }
    }
    return *m_bounding_box;
}

const BoundingBoxf3& Selection::get_unscaled_instance_bounding_box() const
{
    if (!m_unscaled_instance_bounding_box.has_value()) {
        std::optional<BoundingBoxf3>* bbox = const_cast<std::optional<BoundingBoxf3>*>(&m_unscaled_instance_bounding_box);
        *bbox = BoundingBoxf3();
        if (m_valid) {
            for (unsigned int i : m_list) {
                const GLVolume& volume = *(*m_volumes)[i];
                if (volume.is_modifier)
                    continue;
                Transform3d trafo = volume.get_instance_transformation().get_matrix(false, false, true, false) * volume.get_volume_transformation().get_matrix();
                trafo.translation().z() += volume.get_sla_shift_z();
                (*bbox)->merge(volume.transformed_convex_hull_bounding_box(trafo));
            }
        }
    }
    return *m_unscaled_instance_bounding_box;
}

const BoundingBoxf3& Selection::get_scaled_instance_bounding_box() const
{
    if (!m_scaled_instance_bounding_box.has_value()) {
        std::optional<BoundingBoxf3>* bbox = const_cast<std::optional<BoundingBoxf3>*>(&m_scaled_instance_bounding_box);
        *bbox = BoundingBoxf3();
        if (m_valid) {
            for (unsigned int i : m_list) {
                const GLVolume& volume = *(*m_volumes)[i];
                if (volume.is_modifier)
                    continue;
                Transform3d trafo = volume.get_instance_transformation().get_matrix(false, false, false, false) * volume.get_volume_transformation().get_matrix();
                trafo.translation().z() += volume.get_sla_shift_z();
                (*bbox)->merge(volume.transformed_convex_hull_bounding_box(trafo));
            }
        }
    }
    return *m_scaled_instance_bounding_box;
}

void Selection::setup_cache()
{
    if (!m_valid)
        return;

    set_caches();
}

void Selection::translate(const Vec3d& displacement, bool local)
{
    if (!m_valid)
        return;

    EMode translation_type = m_mode;

    for (unsigned int i : m_list) {
        GLVolume& v = *(*m_volumes)[i];
        if (m_mode == Volume || v.is_wipe_tower) {
            if (local)
                v.set_volume_offset(m_cache.volumes_data[i].get_volume_position() + displacement);
            else {
#if ENABLE_WORLD_COORDINATE
                const VolumeCache& volume_data = m_cache.volumes_data[i];
                const Vec3d local_displacement = (volume_data.get_instance_rotation_matrix() * volume_data.get_instance_scale_matrix() * volume_data.get_instance_mirror_matrix()).inverse() * displacement;
                v.set_volume_offset(volume_data.get_volume_position() + local_displacement);
#else
                const Vec3d local_displacement = (m_cache.volumes_data[i].get_instance_rotation_matrix() * m_cache.volumes_data[i].get_instance_scale_matrix() * m_cache.volumes_data[i].get_instance_mirror_matrix()).inverse() * displacement;
                v.set_volume_offset(m_cache.volumes_data[i].get_volume_position() + local_displacement);
#endif // ENABLE_WORLD_COORDINATE
            }
        }
        else if (m_mode == Instance) {
#if ENABLE_WORLD_COORDINATE
            if (is_from_fully_selected_instance(i)) {
                if (local) {
                    const VolumeCache& volume_data = m_cache.volumes_data[i];
                    const Vec3d world_displacement = (volume_data.get_instance_rotation_matrix() * volume_data.get_instance_mirror_matrix()) * displacement;
                    v.set_instance_offset(volume_data.get_instance_position() + world_displacement);
                }
                else
                    v.set_instance_offset(m_cache.volumes_data[i].get_instance_position() + displacement);
            }
#else
            if (is_from_fully_selected_instance(i))
                v.set_instance_offset(m_cache.volumes_data[i].get_instance_position() + displacement);
#endif // ENABLE_WORLD_COORDINATE
            else {
                const Vec3d local_displacement = (m_cache.volumes_data[i].get_instance_rotation_matrix() * m_cache.volumes_data[i].get_instance_scale_matrix() * m_cache.volumes_data[i].get_instance_mirror_matrix()).inverse() * displacement;
                v.set_volume_offset(m_cache.volumes_data[i].get_volume_position() + local_displacement);
                translation_type = Volume;
            }
        }
    }

#if !DISABLE_INSTANCES_SYNCH
    if (translation_type == Instance)
        synchronize_unselected_instances(SyncRotationType::NONE);
    else if (translation_type == Volume)
        synchronize_unselected_volumes();
#endif // !DISABLE_INSTANCES_SYNCH

    ensure_not_below_bed();
    set_bounding_boxes_dirty();
    wxGetApp().plater()->canvas3D()->requires_check_outside_state();
}

// Rotate an object around one of the axes. Only one rotation component is expected to be changing.
void Selection::rotate(const Vec3d& rotation, TransformationType transformation_type)
{
    if (!m_valid)
        return;

    // Only relative rotation values are allowed in the world coordinate system.
    assert(!transformation_type.world() || transformation_type.relative());

    if (!is_wipe_tower()) {
        int rot_axis_max = 0;
        if (rotation.isApprox(Vec3d::Zero())) {
            for (unsigned int i : m_list) {
                GLVolume &v = *(*m_volumes)[i];
                if (m_mode == Instance) {
                    v.set_instance_rotation(m_cache.volumes_data[i].get_instance_rotation());
                    v.set_instance_offset(m_cache.volumes_data[i].get_instance_position());
                }
                else if (m_mode == Volume) {
                    v.set_volume_rotation(m_cache.volumes_data[i].get_volume_rotation());
                    v.set_volume_offset(m_cache.volumes_data[i].get_volume_position());
                }
            }
        }
        else { // this is not the wipe tower
            //FIXME this does not work for absolute rotations (transformation_type.absolute() is true)
            rotation.cwiseAbs().maxCoeff(&rot_axis_max);

//            if ( single instance or single volume )
                // Rotate around center , if only a single object or volume
//                transformation_type.set_independent();

            // For generic rotation, we want to rotate the first volume in selection, and then to synchronize the other volumes with it.
            std::vector<int> object_instance_first(m_model->objects.size(), -1);
            auto rotate_instance = [this, &rotation, &object_instance_first, rot_axis_max, transformation_type](GLVolume &volume, int i) {
                const int first_volume_idx = object_instance_first[volume.object_idx()];
                if (rot_axis_max != 2 && first_volume_idx != -1) {
                    // Generic rotation, but no rotation around the Z axis.
                    // Always do a local rotation (do not consider the selection to be a rigid body).
                    assert(is_approx(rotation.z(), 0.0));
                    const GLVolume &first_volume = *(*m_volumes)[first_volume_idx];
                    const Vec3d    &rotation = first_volume.get_instance_rotation();
                    const double z_diff = Geometry::rotation_diff_z(m_cache.volumes_data[first_volume_idx].get_instance_rotation(), m_cache.volumes_data[i].get_instance_rotation());
                    volume.set_instance_rotation(Vec3d(rotation.x(), rotation.y(), rotation.z() + z_diff));
                }
                else {
                    // extracts rotations from the composed transformation
#if ENABLE_WORLD_COORDINATE
                    const Transform3d m = Geometry::assemble_transform(Vec3d::Zero(), rotation);
                    const Vec3d new_rotation = transformation_type.world() ?
                        Geometry::extract_euler_angles(m * m_cache.volumes_data[i].get_instance_rotation_matrix()) :
                        transformation_type.absolute() ? rotation : Geometry::extract_euler_angles(m_cache.volumes_data[i].get_instance_rotation_matrix() * m);
                    if (rot_axis_max == 2 && transformation_type.joint()) {
                        // Only allow rotation of multiple instances as a single rigid body when rotating around the Z axis.
                        const double z_diff = Geometry::rotation_diff_z(m_cache.volumes_data[i].get_instance_rotation(), new_rotation);
                        volume.set_instance_offset(m_cache.dragging_center + Eigen::AngleAxisd(z_diff, Vec3d::UnitZ()) * (m_cache.volumes_data[i].get_instance_position() - m_cache.dragging_center));
                    }
#else
                    const Vec3d new_rotation = transformation_type.world() ?
                        Geometry::extract_euler_angles(Geometry::assemble_transform(Vec3d::Zero(), rotation) * m_cache.volumes_data[i].get_instance_rotation_matrix()) :
                        transformation_type.absolute() ? rotation : rotation + m_cache.volumes_data[i].get_instance_rotation();
                    if (rot_axis_max == 2 && transformation_type.joint()) {
                        // Only allow rotation of multiple instances as a single rigid body when rotating around the Z axis.
                        const double z_diff = Geometry::rotation_diff_z(m_cache.volumes_data[i].get_instance_rotation(), new_rotation);
                        volume.set_instance_offset(m_cache.dragging_center + Eigen::AngleAxisd(z_diff, Vec3d::UnitZ()) * (m_cache.volumes_data[i].get_instance_position() - m_cache.dragging_center));
                    }
#endif // ENABLE_WORLD_COORDINATE
                    else if (!(m_cache.volumes_data[i].get_instance_position() - m_cache.dragging_center).isApprox(Vec3d::Zero()))
                        volume.set_instance_offset(m_cache.dragging_center + Geometry::assemble_transform(Vec3d::Zero(), new_rotation) * m_cache.volumes_data[i].get_instance_rotation_matrix().inverse() * (m_cache.volumes_data[i].get_instance_position() - m_cache.dragging_center));

                    volume.set_instance_rotation(new_rotation);
                    object_instance_first[volume.object_idx()] = i;
                }
            };

            for (unsigned int i : m_list) {
                GLVolume &v = *(*m_volumes)[i];
                if (is_single_full_instance())
                    rotate_instance(v, i);
                else if (is_single_volume() || is_single_modifier()) {
#if ENABLE_WORLD_COORDINATE
                    if (transformation_type.local())
                        v.set_volume_rotation(m_cache.volumes_data[i].get_volume_rotation() + rotation);
                    else {
                        Transform3d m = Geometry::assemble_transform(Vec3d::Zero(), rotation);
                        m = m * m_cache.volumes_data[i].get_instance_rotation_matrix();
                        m = m * m_cache.volumes_data[i].get_volume_rotation_matrix();
                        m = m_cache.volumes_data[i].get_instance_rotation_matrix().inverse() * m;
                        v.set_volume_rotation(Geometry::extract_euler_angles(m));
                    }
#else
                    if (transformation_type.independent())
                        v.set_volume_rotation(v.get_volume_rotation() + rotation);
                    else {
                        const Transform3d m = Geometry::assemble_transform(Vec3d::Zero(), rotation);
                        const Vec3d new_rotation = Geometry::extract_euler_angles(m * m_cache.volumes_data[i].get_volume_rotation_matrix());
                        v.set_volume_rotation(new_rotation);
                    }
#endif // ENABLE_WORLD_COORDINATE
                }
                else {
                    if (m_mode == Instance)
                        rotate_instance(v, i);
                    else if (m_mode == Volume) {
                        // extracts rotations from the composed transformation
                        const Transform3d m = Geometry::assemble_transform(Vec3d::Zero(), rotation);
                        const Vec3d new_rotation = Geometry::extract_euler_angles(m * m_cache.volumes_data[i].get_volume_rotation_matrix());
                        if (transformation_type.joint()) {
                            const Vec3d local_pivot = m_cache.volumes_data[i].get_instance_full_matrix().inverse() * m_cache.dragging_center;
                            const Vec3d offset = m * (m_cache.volumes_data[i].get_volume_position() - local_pivot);
                            v.set_volume_offset(local_pivot + offset);
                        }
                        v.set_volume_rotation(new_rotation);
                    }
                }
            }
        }

#if !DISABLE_INSTANCES_SYNCH
#if ENABLE_WORLD_COORDINATE
        if (m_mode == Instance) {
            SyncRotationType synch;
            if (transformation_type.world() && rot_axis_max == 2)
                synch = SyncRotationType::NONE;
            else if (transformation_type.local())
                synch = SyncRotationType::FULL;
            else
                synch = SyncRotationType::GENERAL;
            synchronize_unselected_instances(synch);
        }
#else
        if (m_mode == Instance)
            synchronize_unselected_instances((rot_axis_max == 2) ? SyncRotationType::NONE : SyncRotationType::GENERAL);
#endif // ENABLE_WORLD_COORDINATE
        else if (m_mode == Volume)
            synchronize_unselected_volumes();
#endif // !DISABLE_INSTANCES_SYNCH
    }
    else { // it's the wipe tower that's selected and being rotated
        GLVolume& volume = *((*m_volumes)[*m_list.begin()]); // the wipe tower is always alone in the selection

        // make sure the wipe tower rotates around its center, not origin
        // we can assume that only Z rotation changes
        const Vec3d center_local = volume.transformed_bounding_box().center() - volume.get_volume_offset();
        const Vec3d center_local_new = Eigen::AngleAxisd(rotation.z()-volume.get_volume_rotation().z(), Vec3d::UnitZ()) * center_local;
        volume.set_volume_rotation(rotation);
        volume.set_volume_offset(volume.get_volume_offset() + center_local - center_local_new);
    }

    set_bounding_boxes_dirty();
    wxGetApp().plater()->canvas3D()->requires_check_outside_state();
}

void Selection::flattening_rotate(const Vec3d& normal)
{
    // We get the normal in untransformed coordinates. We must transform it using the instance matrix, find out
    // how to rotate the instance so it faces downwards and do the rotation. All that for all selected instances.
    // The function assumes that is_from_single_object() holds.
    assert(Slic3r::is_approx(normal.norm(), 1.));

    if (!m_valid)
        return;

    for (unsigned int i : m_list) {
        GLVolume& v = *(*m_volumes)[i];
        // Normal transformed from the object coordinate space to the world coordinate space.
        const auto &voldata = m_cache.volumes_data[i];
        Vec3d tnormal = (Geometry::assemble_transform(
            Vec3d::Zero(), voldata.get_instance_rotation(), 
            voldata.get_instance_scaling_factor().cwiseInverse(), voldata.get_instance_mirror()) * normal).normalized();
        // Additional rotation to align tnormal with the down vector in the world coordinate space.
        auto  extra_rotation = Eigen::Quaterniond().setFromTwoVectors(tnormal, - Vec3d::UnitZ());
        v.set_instance_rotation(Geometry::extract_euler_angles(extra_rotation.toRotationMatrix() * m_cache.volumes_data[i].get_instance_rotation_matrix()));
    }

#if !DISABLE_INSTANCES_SYNCH
    // Apply the same transformation also to other instances,
    // but respect their possibly diffrent z-rotation.
    if (m_mode == Instance)
        synchronize_unselected_instances(SyncRotationType::GENERAL);
#endif // !DISABLE_INSTANCES_SYNCH

    this->set_bounding_boxes_dirty();
}

void Selection::scale(const Vec3d& scale, TransformationType transformation_type)
{
    if (!m_valid)
        return;

    for (unsigned int i : m_list) {
        GLVolume &v = *(*m_volumes)[i];
        if (is_single_full_instance()) {
            if (transformation_type.relative()) {
                const Transform3d m = Geometry::assemble_transform(Vec3d::Zero(), Vec3d::Zero(), scale);
                const Eigen::Matrix<double, 3, 3, Eigen::DontAlign> new_matrix = (m * m_cache.volumes_data[i].get_instance_scale_matrix()).matrix().block(0, 0, 3, 3);
                // extracts scaling factors from the composed transformation
                const Vec3d new_scale(new_matrix.col(0).norm(), new_matrix.col(1).norm(), new_matrix.col(2).norm());
                if (transformation_type.joint())
                    v.set_instance_offset(m_cache.dragging_center + m * (m_cache.volumes_data[i].get_instance_position() - m_cache.dragging_center));

                v.set_instance_scaling_factor(new_scale);
            }
            else {
#if ENABLE_WORLD_COORDINATE
                v.set_instance_scaling_factor(scale);
#else
                if (transformation_type.world() && (std::abs(scale.x() - scale.y()) > EPSILON || std::abs(scale.x() - scale.z()) > EPSILON)) {
                    // Non-uniform scaling. Transform the scaling factors into the local coordinate system.
                    // This is only possible, if the instance rotation is mulitples of ninety degrees.
                    assert(Geometry::is_rotation_ninety_degrees(v.get_instance_rotation()));
                    v.set_instance_scaling_factor((v.get_instance_transformation().get_matrix(true, false, true, true).matrix().block<3, 3>(0, 0).transpose() * scale).cwiseAbs());
                }
                else
                    v.set_instance_scaling_factor(scale);
#endif // ENABLE_WORLD_COORDINATE
            }
        }
        else if (is_single_volume() || is_single_modifier())
            v.set_volume_scaling_factor(scale);
        else {
            const Transform3d m = Geometry::assemble_transform(Vec3d::Zero(), Vec3d::Zero(), scale);
            if (m_mode == Instance) {
                const Eigen::Matrix<double, 3, 3, Eigen::DontAlign> new_matrix = (m * m_cache.volumes_data[i].get_instance_scale_matrix()).matrix().block(0, 0, 3, 3);
                // extracts scaling factors from the composed transformation
                const Vec3d new_scale(new_matrix.col(0).norm(), new_matrix.col(1).norm(), new_matrix.col(2).norm());
                if (transformation_type.joint())
                    v.set_instance_offset(m_cache.dragging_center + m * (m_cache.volumes_data[i].get_instance_position() - m_cache.dragging_center));

                v.set_instance_scaling_factor(new_scale);
            }
            else if (m_mode == Volume) {
                const Eigen::Matrix<double, 3, 3, Eigen::DontAlign> new_matrix = (m * m_cache.volumes_data[i].get_volume_scale_matrix()).matrix().block(0, 0, 3, 3);
                // extracts scaling factors from the composed transformation
                const Vec3d new_scale(new_matrix.col(0).norm(), new_matrix.col(1).norm(), new_matrix.col(2).norm());
                if (transformation_type.joint()) {
                    const Vec3d offset = m * (m_cache.volumes_data[i].get_volume_position() + m_cache.volumes_data[i].get_instance_position() - m_cache.dragging_center);
                    v.set_volume_offset(m_cache.dragging_center - m_cache.volumes_data[i].get_instance_position() + offset);
                }
                v.set_volume_scaling_factor(new_scale);
            }
        }
    }

#if !DISABLE_INSTANCES_SYNCH
    if (m_mode == Instance)
        synchronize_unselected_instances(SyncRotationType::NONE);
    else if (m_mode == Volume)
        synchronize_unselected_volumes();
#endif // !DISABLE_INSTANCES_SYNCH

    ensure_on_bed();
    set_bounding_boxes_dirty();
    wxGetApp().plater()->canvas3D()->requires_check_outside_state();
}

void Selection::scale_to_fit_print_volume(const BuildVolume& volume)
{
    auto fit = [this](double s, Vec3d offset) {
        if (s <= 0.0 || s == 1.0)
            return;

        wxGetApp().plater()->take_snapshot(_L("Scale To Fit"));

        TransformationType type;
        type.set_world();
        type.set_relative();
        type.set_joint();

        // apply scale
        setup_cache();
        scale(s * Vec3d::Ones(), type);
        wxGetApp().plater()->canvas3D()->do_scale(""); // avoid storing another snapshot

        // center selection on print bed
        setup_cache();
        offset.z() = -get_bounding_box().min.z();
        translate(offset);
        wxGetApp().plater()->canvas3D()->do_move(""); // avoid storing another snapshot

        wxGetApp().obj_manipul()->set_dirty();
    };

    auto fit_rectangle = [this, fit](const BuildVolume& volume) {
        const BoundingBoxf3 print_volume = volume.bounding_volume();
        const Vec3d print_volume_size = print_volume.size();

        // adds 1/100th of a mm on all sides to avoid false out of print volume detections due to floating-point roundings
        const Vec3d box_size = get_bounding_box().size() + 0.02 * Vec3d::Ones();

        const double sx = (box_size.x() != 0.0) ? print_volume_size.x() / box_size.x() : 0.0;
        const double sy = (box_size.y() != 0.0) ? print_volume_size.y() / box_size.y() : 0.0;
        const double sz = (box_size.z() != 0.0) ? print_volume_size.z() / box_size.z() : 0.0;

        if (sx != 0.0 && sy != 0.0 && sz != 0.0)
            fit(std::min(sx, std::min(sy, sz)), print_volume.center() - get_bounding_box().center());
    };

    auto fit_circle = [this, fit](const BuildVolume& volume) {
        const Geometry::Circled& print_circle = volume.circle();
        double print_circle_radius = unscale<double>(print_circle.radius);

        if (print_circle_radius == 0.0)
            return;

        Points points;
        double max_z = 0.0;
        for (unsigned int i : m_list) {
            const GLVolume& v = *(*m_volumes)[i];
            TriangleMesh hull_3d = *v.convex_hull();
            hull_3d.transform(v.world_matrix());
            max_z = std::max(max_z, hull_3d.bounding_box().size().z());
            const Polygon hull_2d = hull_3d.convex_hull();
            points.insert(points.end(), hull_2d.begin(), hull_2d.end());
        }

        if (points.empty())
            return;

        const Geometry::Circled circle = Geometry::smallest_enclosing_circle_welzl(points);
        // adds 1/100th of a mm on all sides to avoid false out of print volume detections due to floating-point roundings
        const double circle_radius = unscale<double>(circle.radius) + 0.01;

        if (circle_radius == 0.0 || max_z == 0.0)
            return;

        const double s = std::min(print_circle_radius / circle_radius, volume.max_print_height() / max_z);
        const Vec3d sel_center = get_bounding_box().center();
        const Vec3d offset = s * (Vec3d(unscale<double>(circle.center.x()), unscale<double>(circle.center.y()), 0.5 * max_z) - sel_center);
        const Vec3d print_center = { unscale<double>(print_circle.center.x()), unscale<double>(print_circle.center.y()), 0.5 * volume.max_print_height() };
        fit(s, print_center - (sel_center + offset));
    };

    if (is_empty() || m_mode == Volume)
        return;

    switch (volume.type())
    {
    case BuildVolume::Type::Rectangle: { fit_rectangle(volume); break; }
    case BuildVolume::Type::Circle:    { fit_circle(volume); break; }
    default: { break; }
    }
}

void Selection::mirror(Axis axis)
{
    if (!m_valid)
        return;

    for (unsigned int i : m_list) {
        GLVolume& v = *(*m_volumes)[i];
        if (is_single_full_instance())
            v.set_instance_mirror(axis, -v.get_instance_mirror(axis));
        else if (m_mode == Volume)
            v.set_volume_mirror(axis, -v.get_volume_mirror(axis));
    }

#if !DISABLE_INSTANCES_SYNCH
    if (m_mode == Instance)
        synchronize_unselected_instances(SyncRotationType::NONE);
    else if (m_mode == Volume)
        synchronize_unselected_volumes();
#endif // !DISABLE_INSTANCES_SYNCH

    set_bounding_boxes_dirty();
}

void Selection::translate(unsigned int object_idx, const Vec3d& displacement)
{
    if (!m_valid)
        return;

    for (unsigned int i : m_list) {
        GLVolume& v = *(*m_volumes)[i];
        if (v.object_idx() == (int)object_idx)
            v.set_instance_offset(v.get_instance_offset() + displacement);
    }

    std::set<unsigned int> done;  // prevent processing volumes twice
    done.insert(m_list.begin(), m_list.end());

    for (unsigned int i : m_list) {
        if (done.size() == m_volumes->size())
            break;

#if ENABLE_WIPETOWER_OBJECTID_1000_REMOVAL
        if ((*m_volumes)[i]->is_wipe_tower)
            continue;

        int object_idx = (*m_volumes)[i]->object_idx();
#else
        int object_idx = (*m_volumes)[i]->object_idx();
        if (object_idx >= 1000)
            continue;
#endif // ENABLE_WIPETOWER_OBJECTID_1000_REMOVAL

        // Process unselected volumes of the object.
        for (unsigned int j = 0; j < (unsigned int)m_volumes->size(); ++j) {
            if (done.size() == m_volumes->size())
                break;

            if (done.find(j) != done.end())
                continue;

            GLVolume& v = *(*m_volumes)[j];
            if (v.object_idx() != object_idx)
                continue;

            v.set_instance_offset(v.get_instance_offset() + displacement);
            done.insert(j);
        }
    }

    this->set_bounding_boxes_dirty();
}

void Selection::translate(unsigned int object_idx, unsigned int instance_idx, const Vec3d& displacement)
{
    if (!m_valid)
        return;

    for (unsigned int i : m_list) {
        GLVolume& v = *(*m_volumes)[i];
        if (v.object_idx() == (int)object_idx && v.instance_idx() == (int)instance_idx)
            v.set_instance_offset(v.get_instance_offset() + displacement);
    }

    std::set<unsigned int> done;  // prevent processing volumes twice
    done.insert(m_list.begin(), m_list.end());

    for (unsigned int i : m_list) {
        if (done.size() == m_volumes->size())
            break;

#if ENABLE_WIPETOWER_OBJECTID_1000_REMOVAL
        if ((*m_volumes)[i]->is_wipe_tower)
            continue;

        int object_idx = (*m_volumes)[i]->object_idx();
#else
        int object_idx = (*m_volumes)[i]->object_idx();
        if (object_idx >= 1000)
            continue;
#endif // ENABLE_WIPETOWER_OBJECTID_1000_REMOVAL

        // Process unselected volumes of the object.
        for (unsigned int j = 0; j < (unsigned int)m_volumes->size(); ++j) {
            if (done.size() == m_volumes->size())
                break;

            if (done.find(j) != done.end())
                continue;

            GLVolume& v = *(*m_volumes)[j];
            if (v.object_idx() != object_idx || v.instance_idx() != (int)instance_idx)
                continue;

            v.set_instance_offset(v.get_instance_offset() + displacement);
            done.insert(j);
        }
    }

    this->set_bounding_boxes_dirty();
}

void Selection::erase()
{
    if (!m_valid)
        return;

    if (is_single_full_object())
        wxGetApp().obj_list()->delete_from_model_and_list(ItemType::itObject, get_object_idx(), 0);
    else if (is_multiple_full_object()) {
        std::vector<ItemForDelete> items;
        items.reserve(m_cache.content.size());
        for (ObjectIdxsToInstanceIdxsMap::iterator it = m_cache.content.begin(); it != m_cache.content.end(); ++it) {
            items.emplace_back(ItemType::itObject, it->first, 0);
        }
        wxGetApp().obj_list()->delete_from_model_and_list(items);
    }
    else if (is_multiple_full_instance()) {
        std::set<std::pair<int, int>> instances_idxs;
        for (ObjectIdxsToInstanceIdxsMap::iterator obj_it = m_cache.content.begin(); obj_it != m_cache.content.end(); ++obj_it) {
            for (InstanceIdxsList::reverse_iterator inst_it = obj_it->second.rbegin(); inst_it != obj_it->second.rend(); ++inst_it) {
                instances_idxs.insert(std::make_pair(obj_it->first, *inst_it));
            }
        }

        std::vector<ItemForDelete> items;
        items.reserve(instances_idxs.size());
        for (const std::pair<int, int>& i : instances_idxs) {
            items.emplace_back(ItemType::itInstance, i.first, i.second);
        }
        wxGetApp().obj_list()->delete_from_model_and_list(items);
    }
    else if (is_single_full_instance())
        wxGetApp().obj_list()->delete_from_model_and_list(ItemType::itInstance, get_object_idx(), get_instance_idx());
    else if (is_mixed()) {
        std::set<ItemForDelete> items_set;
        std::map<int, int> volumes_in_obj;

        for (auto i : m_list) {
            const auto gl_vol = (*m_volumes)[i];
            const auto glv_obj_idx = gl_vol->object_idx();
            const auto model_object = m_model->objects[glv_obj_idx];

            if (model_object->instances.size() == 1) {
                if (model_object->volumes.size() == 1)
                    items_set.insert(ItemForDelete(ItemType::itObject, glv_obj_idx, -1));
                else {
                    items_set.insert(ItemForDelete(ItemType::itVolume, glv_obj_idx, gl_vol->volume_idx()));
                    int idx = (volumes_in_obj.find(glv_obj_idx) == volumes_in_obj.end()) ? 0 : volumes_in_obj.at(glv_obj_idx);
                    volumes_in_obj[glv_obj_idx] = ++idx;
                }
                continue;
            }

            const auto glv_ins_idx = gl_vol->instance_idx();

            for (auto obj_ins : m_cache.content) {
                if (obj_ins.first == glv_obj_idx) {
                    if (obj_ins.second.find(glv_ins_idx) != obj_ins.second.end()) {
                        if (obj_ins.second.size() == model_object->instances.size())
                            items_set.insert(ItemForDelete(ItemType::itObject, glv_obj_idx, -1));
                        else
                            items_set.insert(ItemForDelete(ItemType::itInstance, glv_obj_idx, glv_ins_idx));

                        break;
                    }
                }
            }
        }

        std::vector<ItemForDelete> items;
        items.reserve(items_set.size());
        for (const ItemForDelete& i : items_set) {
            if (i.type == ItemType::itVolume) {
                const int vol_in_obj_cnt = volumes_in_obj.find(i.obj_idx) == volumes_in_obj.end() ? 0 : volumes_in_obj.at(i.obj_idx);
                if (vol_in_obj_cnt == (int)m_model->objects[i.obj_idx]->volumes.size()) {
                    if (i.sub_obj_idx == vol_in_obj_cnt - 1)
                        items.emplace_back(ItemType::itObject, i.obj_idx, 0);
                    continue;
                }
            }
            items.emplace_back(i.type, i.obj_idx, i.sub_obj_idx);
        }

        wxGetApp().obj_list()->delete_from_model_and_list(items);
    }
    else {
        std::set<std::pair<int, int>> volumes_idxs;
        for (unsigned int i : m_list) {
            const GLVolume* v = (*m_volumes)[i];
            // Only remove volumes associated with ModelVolumes from the object list.
            // Temporary meshes (SLA supports or pads) are not managed by the object list.
            if (v->volume_idx() >= 0)
                volumes_idxs.insert(std::make_pair(v->object_idx(), v->volume_idx()));
        }

        std::vector<ItemForDelete> items;
        items.reserve(volumes_idxs.size());
        for (const std::pair<int, int>& v : volumes_idxs) {
            items.emplace_back(ItemType::itVolume, v.first, v.second);
        }

        wxGetApp().obj_list()->delete_from_model_and_list(items);
        ensure_not_below_bed();
    }
}

void Selection::render(float scale_factor)
{
    if (!m_valid || is_empty())
        return;

    m_scale_factor = scale_factor;
    // render cumulative bounding box of selected volumes
#if ENABLE_LEGACY_OPENGL_REMOVAL
    render_bounding_box(get_bounding_box(), ColorRGB::WHITE());
#else
    render_selected_volumes();
#endif // ENABLE_LEGACY_OPENGL_REMOVAL
    render_synchronized_volumes();
}

#if ENABLE_RENDER_SELECTION_CENTER
void Selection::render_center(bool gizmo_is_dragging)
{
    if (!m_valid || is_empty())
        return;

#if ENABLE_LEGACY_OPENGL_REMOVAL
    GLShaderProgram* shader = wxGetApp().get_shader("flat");
    if (shader == nullptr)
        return;

    shader->start_using();
#endif // ENABLE_LEGACY_OPENGL_REMOVAL

    const Vec3d center = gizmo_is_dragging ? m_cache.dragging_center : get_bounding_box().center();

    glsafe(::glDisable(GL_DEPTH_TEST));

#if ENABLE_GL_SHADERS_ATTRIBUTES
    const Camera& camera = wxGetApp().plater()->get_camera();
    Transform3d view_model_matrix = camera.get_view_matrix() * Geometry::assemble_transform(center);

    shader->set_uniform("view_model_matrix", view_model_matrix);
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());
#else
    glsafe(::glPushMatrix());
    glsafe(::glTranslated(center.x(), center.y(), center.z()));
#endif // ENABLE_GL_SHADERS_ATTRIBUTES

#if ENABLE_LEGACY_OPENGL_REMOVAL
    m_vbo_sphere.set_color(ColorRGBA::WHITE());
#else
    m_vbo_sphere.set_color(-1, ColorRGBA::WHITE());
#endif // ENABLE_LEGACY_OPENGL_REMOVAL
    m_vbo_sphere.render();

#if !ENABLE_GL_SHADERS_ATTRIBUTES
    glsafe(::glPopMatrix());
#endif // !ENABLE_GL_SHADERS_ATTRIBUTES

#if ENABLE_LEGACY_OPENGL_REMOVAL
    shader->stop_using();
#endif // ENABLE_LEGACY_OPENGL_REMOVAL
}
#endif // ENABLE_RENDER_SELECTION_CENTER

void Selection::render_sidebar_hints(const std::string& sidebar_field)
{
    if (sidebar_field.empty())
        return;

#if ENABLE_LEGACY_OPENGL_REMOVAL
    GLShaderProgram* shader = wxGetApp().get_shader(boost::starts_with(sidebar_field, "layer") ? "flat" : "gouraud_light");
    if (shader == nullptr)
        return;

    shader->start_using();
#else
    GLShaderProgram* shader = nullptr;

    if (!boost::starts_with(sidebar_field, "layer")) {
        shader = wxGetApp().get_shader("gouraud_light");
        if (shader == nullptr)
            return;

        shader->start_using();
        glsafe(::glClear(GL_DEPTH_BUFFER_BIT));
    }
#endif // ENABLE_LEGACY_OPENGL_REMOVAL

    glsafe(::glEnable(GL_DEPTH_TEST));

#if ENABLE_GL_SHADERS_ATTRIBUTES
    const Transform3d base_matrix = Geometry::assemble_transform(get_bounding_box().center());
    Transform3d orient_matrix = Transform3d::Identity();
#else
    glsafe(::glPushMatrix());
#endif // ENABLE_GL_SHADERS_ATTRIBUTES

    if (!boost::starts_with(sidebar_field, "layer")) {
#if ENABLE_GL_SHADERS_ATTRIBUTES
        shader->set_uniform("emission_factor", 0.05f);
#else
        const Vec3d& center = get_bounding_box().center();
#endif // ENABLE_GL_SHADERS_ATTRIBUTES
        if (is_single_full_instance() && !wxGetApp().obj_manipul()->get_world_coordinates()) {
#if !ENABLE_GL_SHADERS_ATTRIBUTES
            glsafe(::glTranslated(center.x(), center.y(), center.z()));
#endif // !ENABLE_GL_SHADERS_ATTRIBUTES
            if (!boost::starts_with(sidebar_field, "position")) {
#if !ENABLE_GL_SHADERS_ATTRIBUTES
                Transform3d orient_matrix = Transform3d::Identity();
#endif // !ENABLE_GL_SHADERS_ATTRIBUTES
                if (boost::starts_with(sidebar_field, "scale"))
                    orient_matrix = (*m_volumes)[*m_list.begin()]->get_instance_transformation().get_matrix(true, false, true, true);
                else if (boost::starts_with(sidebar_field, "rotation")) {
                    if (boost::ends_with(sidebar_field, "x"))
                        orient_matrix = (*m_volumes)[*m_list.begin()]->get_instance_transformation().get_matrix(true, false, true, true);
                    else if (boost::ends_with(sidebar_field, "y")) {
                        const Vec3d& rotation = (*m_volumes)[*m_list.begin()]->get_instance_transformation().get_rotation();
                        if (rotation.x() == 0.0)
                            orient_matrix = (*m_volumes)[*m_list.begin()]->get_instance_transformation().get_matrix(true, false, true, true);
                        else
                            orient_matrix.rotate(Eigen::AngleAxisd(rotation.z(), Vec3d::UnitZ()));
                    }
                }
#if !ENABLE_GL_SHADERS_ATTRIBUTES
                glsafe(::glMultMatrixd(orient_matrix.data()));
#endif // !ENABLE_GL_SHADERS_ATTRIBUTES
            }
        }
        else if (is_single_volume() || is_single_modifier()) {
#if ENABLE_GL_SHADERS_ATTRIBUTES
            orient_matrix = (*m_volumes)[*m_list.begin()]->get_instance_transformation().get_matrix(true, false, true, true);
#else
            glsafe(::glTranslated(center.x(), center.y(), center.z()));
            Transform3d orient_matrix = (*m_volumes)[*m_list.begin()]->get_instance_transformation().get_matrix(true, false, true, true);
#endif // ENABLE_GL_SHADERS_ATTRIBUTES
            if (!boost::starts_with(sidebar_field, "position"))
                orient_matrix = orient_matrix * (*m_volumes)[*m_list.begin()]->get_volume_transformation().get_matrix(true, false, true, true);

#if !ENABLE_GL_SHADERS_ATTRIBUTES
            glsafe(::glMultMatrixd(orient_matrix.data()));
#endif // !ENABLE_GL_SHADERS_ATTRIBUTES
        }
        else {
#if ENABLE_GL_SHADERS_ATTRIBUTES
            if (requires_local_axes())
                orient_matrix = (*m_volumes)[*m_list.begin()]->get_instance_transformation().get_matrix(true, false, true, true);
#else
            glsafe(::glTranslated(center.x(), center.y(), center.z()));
            if (requires_local_axes()) {
                const Transform3d orient_matrix = (*m_volumes)[*m_list.begin()]->get_instance_transformation().get_matrix(true, false, true, true);
                glsafe(::glMultMatrixd(orient_matrix.data()));
            }
#endif // ENABLE_GL_SHADERS_ATTRIBUTES
        }
    }

#if ENABLE_LEGACY_OPENGL_REMOVAL
    if (!boost::starts_with(sidebar_field, "layer"))
        glsafe(::glClear(GL_DEPTH_BUFFER_BIT));
#endif // ENABLE_LEGACY_OPENGL_REMOVAL

#if ENABLE_GL_SHADERS_ATTRIBUTES
    if (boost::starts_with(sidebar_field, "position"))
        render_sidebar_position_hints(sidebar_field, *shader, base_matrix * orient_matrix);
    else if (boost::starts_with(sidebar_field, "rotation"))
        render_sidebar_rotation_hints(sidebar_field, *shader, base_matrix * orient_matrix);
    else if (boost::starts_with(sidebar_field, "scale") || boost::starts_with(sidebar_field, "size"))
        render_sidebar_scale_hints(sidebar_field, *shader, base_matrix * orient_matrix);
    else if (boost::starts_with(sidebar_field, "layer"))
        render_sidebar_layers_hints(sidebar_field, *shader);
#else
    if (boost::starts_with(sidebar_field, "position"))
        render_sidebar_position_hints(sidebar_field);
    else if (boost::starts_with(sidebar_field, "rotation"))
        render_sidebar_rotation_hints(sidebar_field);
    else if (boost::starts_with(sidebar_field, "scale") || boost::starts_with(sidebar_field, "size"))
        render_sidebar_scale_hints(sidebar_field);
    else if (boost::starts_with(sidebar_field, "layer"))
        render_sidebar_layers_hints(sidebar_field);

    glsafe(::glPopMatrix());
#endif // ENABLE_GL_SHADERS_ATTRIBUTES

#if !ENABLE_LEGACY_OPENGL_REMOVAL
    if (!boost::starts_with(sidebar_field, "layer"))
#endif // !ENABLE_LEGACY_OPENGL_REMOVAL
        shader->stop_using();
}

bool Selection::requires_local_axes() const
{
    return m_mode == Volume && is_from_single_instance();
}

void Selection::copy_to_clipboard()
{
    if (!m_valid)
        return;

    m_clipboard.reset();

    for (const ObjectIdxsToInstanceIdxsMap::value_type& object : m_cache.content) {
        ModelObject* src_object = m_model->objects[object.first];
        ModelObject* dst_object = m_clipboard.add_object();
        dst_object->name                 = src_object->name;
        dst_object->input_file           = src_object->input_file;
		dst_object->config.assign_config(src_object->config);
        dst_object->sla_support_points   = src_object->sla_support_points;
        dst_object->sla_points_status    = src_object->sla_points_status;
        dst_object->sla_drain_holes      = src_object->sla_drain_holes;
        dst_object->layer_config_ranges  = src_object->layer_config_ranges;     // #ys_FIXME_experiment
        dst_object->layer_height_profile.assign(src_object->layer_height_profile);
        dst_object->origin_translation   = src_object->origin_translation;

        for (int i : object.second) {
            dst_object->add_instance(*src_object->instances[i]);
        }

        for (unsigned int i : m_list) {
            // Copy the ModelVolumes only for the selected GLVolumes of the 1st selected instance.
            const GLVolume* volume = (*m_volumes)[i];
            if (volume->object_idx() == object.first && volume->instance_idx() == *object.second.begin()) {
                int volume_idx = volume->volume_idx();
                if (0 <= volume_idx && volume_idx < (int)src_object->volumes.size()) {
                    ModelVolume* src_volume = src_object->volumes[volume_idx];
                    ModelVolume* dst_volume = dst_object->add_volume(*src_volume);
                    dst_volume->set_new_unique_id();
                }
                else
                    assert(false);
            }
        }
    }

    m_clipboard.set_mode(m_mode);
}

void Selection::paste_from_clipboard()
{
    if (!m_valid || m_clipboard.is_empty())
        return;

    switch (m_clipboard.get_mode())
    {
    case Volume:
    {
        if (is_from_single_instance())
            paste_volumes_from_clipboard();

        break;
    }
    case Instance:
    {
        if (m_mode == Instance)
            paste_objects_from_clipboard();

        break;
    }
    }
}

std::vector<unsigned int> Selection::get_volume_idxs_from_object(unsigned int object_idx) const
{
    std::vector<unsigned int> idxs;

    for (unsigned int i = 0; i < (unsigned int)m_volumes->size(); ++i)
    {
        if ((*m_volumes)[i]->object_idx() == (int)object_idx)
            idxs.push_back(i);
    }

    return idxs;
}

std::vector<unsigned int> Selection::get_volume_idxs_from_instance(unsigned int object_idx, unsigned int instance_idx) const
{
    std::vector<unsigned int> idxs;

    for (unsigned int i = 0; i < (unsigned int)m_volumes->size(); ++i)
    {
        const GLVolume* v = (*m_volumes)[i];
        if ((v->object_idx() == (int)object_idx) && (v->instance_idx() == (int)instance_idx))
            idxs.push_back(i);
    }

    return idxs;
}

std::vector<unsigned int> Selection::get_volume_idxs_from_volume(unsigned int object_idx, unsigned int instance_idx, unsigned int volume_idx) const
{
    std::vector<unsigned int> idxs;

    for (unsigned int i = 0; i < (unsigned int)m_volumes->size(); ++i)
    {
        const GLVolume* v = (*m_volumes)[i];
        if ((v->object_idx() == (int)object_idx) && (v->volume_idx() == (int)volume_idx))
        {
            if (((int)instance_idx != -1) && (v->instance_idx() == (int)instance_idx))
                idxs.push_back(i);
        }
    }

    return idxs;
}

std::vector<unsigned int> Selection::get_missing_volume_idxs_from(const std::vector<unsigned int>& volume_idxs) const
{
    std::vector<unsigned int> idxs;

    for (unsigned int i : m_list)
    {
        std::vector<unsigned int>::const_iterator it = std::find(volume_idxs.begin(), volume_idxs.end(), i);
        if (it == volume_idxs.end())
            idxs.push_back(i);
    }

    return idxs;
}

std::vector<unsigned int> Selection::get_unselected_volume_idxs_from(const std::vector<unsigned int>& volume_idxs) const
{
    std::vector<unsigned int> idxs;

    for (unsigned int i : volume_idxs)
    {
        if (m_list.find(i) == m_list.end())
            idxs.push_back(i);
    }

    return idxs;
}

void Selection::update_valid()
{
    m_valid = (m_volumes != nullptr) && (m_model != nullptr);
}

void Selection::update_type()
{
    m_cache.content.clear();
    m_type = Mixed;

    for (unsigned int i : m_list)
    {
        const GLVolume* volume = (*m_volumes)[i];
        int obj_idx = volume->object_idx();
        int inst_idx = volume->instance_idx();
        ObjectIdxsToInstanceIdxsMap::iterator obj_it = m_cache.content.find(obj_idx);
        if (obj_it == m_cache.content.end())
            obj_it = m_cache.content.insert(ObjectIdxsToInstanceIdxsMap::value_type(obj_idx, InstanceIdxsList())).first;

        obj_it->second.insert(inst_idx);
    }

    bool requires_disable = false;

    if (!m_valid)
        m_type = Invalid;
    else
    {
        if (m_list.empty())
            m_type = Empty;
        else if (m_list.size() == 1)
        {
            const GLVolume* first = (*m_volumes)[*m_list.begin()];
            if (first->is_wipe_tower)
                m_type = WipeTower;
            else if (first->is_modifier)
            {
                m_type = SingleModifier;
                requires_disable = true;
            }
            else
            {
                const ModelObject* model_object = m_model->objects[first->object_idx()];
                unsigned int volumes_count = (unsigned int)model_object->volumes.size();
                unsigned int instances_count = (unsigned int)model_object->instances.size();
                if (volumes_count * instances_count == 1)
                {
                    m_type = SingleFullObject;
                    // ensures the correct mode is selected
                    m_mode = Instance;
                }
                else if (volumes_count == 1) // instances_count > 1
                {
                    m_type = SingleFullInstance;
                    // ensures the correct mode is selected
                    m_mode = Instance;
                }
                else
                {
                    m_type = SingleVolume;
                    requires_disable = true;
                }
            }
        }
        else
        {
            unsigned int sla_volumes_count = 0;
            // Note: sla_volumes_count is a count of the selected sla_volumes per object instead of per instance, like a model_volumes_count is
            for (unsigned int i : m_list) {
                if ((*m_volumes)[i]->volume_idx() < 0)
                    ++sla_volumes_count;
            }

            if (m_cache.content.size() == 1) // single object
            {
                const ModelObject* model_object = m_model->objects[m_cache.content.begin()->first];
                unsigned int model_volumes_count = (unsigned int)model_object->volumes.size();

                unsigned int instances_count = (unsigned int)model_object->instances.size();
                unsigned int selected_instances_count = (unsigned int)m_cache.content.begin()->second.size();
                if (model_volumes_count * instances_count + sla_volumes_count == (unsigned int)m_list.size())
                {
                    m_type = SingleFullObject;
                    // ensures the correct mode is selected
                    m_mode = Instance;
                }
                else if (selected_instances_count == 1)
                {
                    if (model_volumes_count + sla_volumes_count == (unsigned int)m_list.size())
                    {
                        m_type = SingleFullInstance;
                        // ensures the correct mode is selected
                        m_mode = Instance;
                    }
                    else
                    {
                        unsigned int modifiers_count = 0;
                        for (unsigned int i : m_list)
                        {
                            if ((*m_volumes)[i]->is_modifier)
                                ++modifiers_count;
                        }

                        if (modifiers_count == 0)
                            m_type = MultipleVolume;
                        else if (modifiers_count == (unsigned int)m_list.size())
                            m_type = MultipleModifier;

                        requires_disable = true;
                    }
                }
                else if ((selected_instances_count > 1) && (selected_instances_count * model_volumes_count + sla_volumes_count == (unsigned int)m_list.size()))
                {
                    m_type = MultipleFullInstance;
                    // ensures the correct mode is selected
                    m_mode = Instance;
                }
            }
            else
            {
                unsigned int sels_cntr = 0;
                for (ObjectIdxsToInstanceIdxsMap::iterator it = m_cache.content.begin(); it != m_cache.content.end(); ++it)
                {
                    const ModelObject* model_object = m_model->objects[it->first];
                    unsigned int volumes_count = (unsigned int)model_object->volumes.size();
                    unsigned int instances_count = (unsigned int)model_object->instances.size();
                    sels_cntr += volumes_count * instances_count;
                }
                if (sels_cntr + sla_volumes_count == (unsigned int)m_list.size())
                {
                    m_type = MultipleFullObject;
                    // ensures the correct mode is selected
                    m_mode = Instance;
                }
            }
        }
    }

    int object_idx = get_object_idx();
    int instance_idx = get_instance_idx();
    for (GLVolume* v : *m_volumes)
    {
        v->disabled = requires_disable ? (v->object_idx() != object_idx) || (v->instance_idx() != instance_idx) : false;
    }

#if ENABLE_SELECTION_DEBUG_OUTPUT
    std::cout << "Selection: ";
    std::cout << "mode: ";
    switch (m_mode)
    {
    case Volume:
    {
        std::cout << "Volume";
        break;
    }
    case Instance:
    {
        std::cout << "Instance";
        break;
    }
    }

    std::cout << " - type: ";

    switch (m_type)
    {
    case Invalid:
    {
        std::cout << "Invalid" << std::endl;
        break;
    }
    case Empty:
    {
        std::cout << "Empty" << std::endl;
        break;
    }
    case WipeTower:
    {
        std::cout << "WipeTower" << std::endl;
        break;
    }
    case SingleModifier:
    {
        std::cout << "SingleModifier" << std::endl;
        break;
    }
    case MultipleModifier:
    {
        std::cout << "MultipleModifier" << std::endl;
        break;
    }
    case SingleVolume:
    {
        std::cout << "SingleVolume" << std::endl;
        break;
    }
    case MultipleVolume:
    {
        std::cout << "MultipleVolume" << std::endl;
        break;
    }
    case SingleFullObject:
    {
        std::cout << "SingleFullObject" << std::endl;
        break;
    }
    case MultipleFullObject:
    {
        std::cout << "MultipleFullObject" << std::endl;
        break;
    }
    case SingleFullInstance:
    {
        std::cout << "SingleFullInstance" << std::endl;
        break;
    }
    case MultipleFullInstance:
    {
        std::cout << "MultipleFullInstance" << std::endl;
        break;
    }
    case Mixed:
    {
        std::cout << "Mixed" << std::endl;
        break;
    }
    }
#endif // ENABLE_SELECTION_DEBUG_OUTPUT
}

void Selection::set_caches()
{
    m_cache.volumes_data.clear();
    m_cache.sinking_volumes.clear();
    for (unsigned int i = 0; i < (unsigned int)m_volumes->size(); ++i) {
        const GLVolume& v = *(*m_volumes)[i];
        m_cache.volumes_data.emplace(i, VolumeCache(v.get_volume_transformation(), v.get_instance_transformation()));
        if (v.is_sinking())
            m_cache.sinking_volumes.push_back(i);
    }
    m_cache.dragging_center = get_bounding_box().center();
}

void Selection::do_add_volume(unsigned int volume_idx)
{
    m_list.insert(volume_idx);
    GLVolume* v = (*m_volumes)[volume_idx];
    v->selected = true;
    if (v->hover == GLVolume::HS_Select || v->hover == GLVolume::HS_Deselect)
        v->hover = GLVolume::HS_Hover;
}

void Selection::do_add_volumes(const std::vector<unsigned int>& volume_idxs)
{
    for (unsigned int i : volume_idxs)
    {
        if (i < (unsigned int)m_volumes->size())
            do_add_volume(i);
    }
}

void Selection::do_remove_volume(unsigned int volume_idx)
{
    IndicesList::iterator v_it = m_list.find(volume_idx);
    if (v_it == m_list.end())
        return;

    m_list.erase(v_it);

    (*m_volumes)[volume_idx]->selected = false;
}

void Selection::do_remove_instance(unsigned int object_idx, unsigned int instance_idx)
{
    for (unsigned int i = 0; i < (unsigned int)m_volumes->size(); ++i) {
        GLVolume* v = (*m_volumes)[i];
        if (v->object_idx() == (int)object_idx && v->instance_idx() == (int)instance_idx)
            do_remove_volume(i);
    }
}

void Selection::do_remove_object(unsigned int object_idx)
{
    for (unsigned int i = 0; i < (unsigned int)m_volumes->size(); ++i) {
        GLVolume* v = (*m_volumes)[i];
        if (v->object_idx() == (int)object_idx)
            do_remove_volume(i);
    }
}

#if !ENABLE_LEGACY_OPENGL_REMOVAL
void Selection::render_selected_volumes() const
{
    float color[3] = { 1.0f, 1.0f, 1.0f };
    render_bounding_box(get_bounding_box(), color);
}
#endif // !ENABLE_LEGACY_OPENGL_REMOVAL

void Selection::render_synchronized_volumes()
{
    if (m_mode == Instance)
        return;

#if !ENABLE_LEGACY_OPENGL_REMOVAL
    float color[3] = { 1.0f, 1.0f, 0.0f };
#endif // !ENABLE_LEGACY_OPENGL_REMOVAL

    for (unsigned int i : m_list) {
        const GLVolume& volume = *(*m_volumes)[i];
        int object_idx = volume.object_idx();
        int volume_idx = volume.volume_idx();
        for (unsigned int j = 0; j < (unsigned int)m_volumes->size(); ++j) {
            if (i == j)
                continue;

            const GLVolume& v = *(*m_volumes)[j];
            if (v.object_idx() != object_idx || v.volume_idx() != volume_idx)
                continue;

#if ENABLE_LEGACY_OPENGL_REMOVAL
            render_bounding_box(v.transformed_convex_hull_bounding_box(), ColorRGB::YELLOW());
#else
            render_bounding_box(v.transformed_convex_hull_bounding_box(), color);
#endif // ENABLE_LEGACY_OPENGL_REMOVAL
        }
    }
}

#if ENABLE_LEGACY_OPENGL_REMOVAL
void Selection::render_bounding_box(const BoundingBoxf3& box, const ColorRGB& color)
{
#else
void Selection::render_bounding_box(const BoundingBoxf3 & box, float* color) const
{
    if (color == nullptr)
        return;

    const Vec3f b_min = box.min.cast<float>();
    const Vec3f b_max = box.max.cast<float>();
    const Vec3f size = 0.2f * box.size().cast<float>();

    glsafe(::glEnable(GL_DEPTH_TEST));
    glsafe(::glColor3fv(color));
    glsafe(::glLineWidth(2.0f * m_scale_factor));
#endif // ENABLE_LEGACY_OPENGL_REMOVAL

#if ENABLE_LEGACY_OPENGL_REMOVAL
    const BoundingBoxf3& curr_box = m_box.get_bounding_box();
    if (!m_box.is_initialized() || !is_approx(box.min, curr_box.min) || !is_approx(box.max, curr_box.max)) {
        m_box.reset();

        const Vec3f b_min = box.min.cast<float>();
        const Vec3f b_max = box.max.cast<float>();
        const Vec3f size = 0.2f * box.size().cast<float>();

        GLModel::Geometry init_data;
        init_data.format = { GLModel::Geometry::EPrimitiveType::Lines, GLModel::Geometry::EVertexLayout::P3 };
        init_data.reserve_vertices(48);
        init_data.reserve_indices(48);

        // vertices
        init_data.add_vertex(Vec3f(b_min.x(), b_min.y(), b_min.z()));
        init_data.add_vertex(Vec3f(b_min.x() + size.x(), b_min.y(), b_min.z()));
        init_data.add_vertex(Vec3f(b_min.x(), b_min.y(), b_min.z()));
        init_data.add_vertex(Vec3f(b_min.x(), b_min.y() + size.y(), b_min.z()));
        init_data.add_vertex(Vec3f(b_min.x(), b_min.y(), b_min.z()));
        init_data.add_vertex(Vec3f(b_min.x(), b_min.y(), b_min.z() + size.z()));

        init_data.add_vertex(Vec3f(b_max.x(), b_min.y(), b_min.z()));
        init_data.add_vertex(Vec3f(b_max.x() - size.x(), b_min.y(), b_min.z()));
        init_data.add_vertex(Vec3f(b_max.x(), b_min.y(), b_min.z()));
        init_data.add_vertex(Vec3f(b_max.x(), b_min.y() + size.y(), b_min.z()));
        init_data.add_vertex(Vec3f(b_max.x(), b_min.y(), b_min.z()));
        init_data.add_vertex(Vec3f(b_max.x(), b_min.y(), b_min.z() + size.z()));

        init_data.add_vertex(Vec3f(b_max.x(), b_max.y(), b_min.z()));
        init_data.add_vertex(Vec3f(b_max.x() - size.x(), b_max.y(), b_min.z()));
        init_data.add_vertex(Vec3f(b_max.x(), b_max.y(), b_min.z()));
        init_data.add_vertex(Vec3f(b_max.x(), b_max.y() - size.y(), b_min.z()));
        init_data.add_vertex(Vec3f(b_max.x(), b_max.y(), b_min.z()));
        init_data.add_vertex(Vec3f(b_max.x(), b_max.y(), b_min.z() + size.z()));

        init_data.add_vertex(Vec3f(b_min.x(), b_max.y(), b_min.z()));
        init_data.add_vertex(Vec3f(b_min.x() + size.x(), b_max.y(), b_min.z()));
        init_data.add_vertex(Vec3f(b_min.x(), b_max.y(), b_min.z()));
        init_data.add_vertex(Vec3f(b_min.x(), b_max.y() - size.y(), b_min.z()));
        init_data.add_vertex(Vec3f(b_min.x(), b_max.y(), b_min.z()));
        init_data.add_vertex(Vec3f(b_min.x(), b_max.y(), b_min.z() + size.z()));

        init_data.add_vertex(Vec3f(b_min.x(), b_min.y(), b_max.z()));
        init_data.add_vertex(Vec3f(b_min.x() + size.x(), b_min.y(), b_max.z()));
        init_data.add_vertex(Vec3f(b_min.x(), b_min.y(), b_max.z()));
        init_data.add_vertex(Vec3f(b_min.x(), b_min.y() + size.y(), b_max.z()));
        init_data.add_vertex(Vec3f(b_min.x(), b_min.y(), b_max.z()));
        init_data.add_vertex(Vec3f(b_min.x(), b_min.y(), b_max.z() - size.z()));

        init_data.add_vertex(Vec3f(b_max.x(), b_min.y(), b_max.z()));
        init_data.add_vertex(Vec3f(b_max.x() - size.x(), b_min.y(), b_max.z()));
        init_data.add_vertex(Vec3f(b_max.x(), b_min.y(), b_max.z()));
        init_data.add_vertex(Vec3f(b_max.x(), b_min.y() + size.y(), b_max.z()));
        init_data.add_vertex(Vec3f(b_max.x(), b_min.y(), b_max.z()));
        init_data.add_vertex(Vec3f(b_max.x(), b_min.y(), b_max.z() - size.z()));

        init_data.add_vertex(Vec3f(b_max.x(), b_max.y(), b_max.z()));
        init_data.add_vertex(Vec3f(b_max.x() - size.x(), b_max.y(), b_max.z()));
        init_data.add_vertex(Vec3f(b_max.x(), b_max.y(), b_max.z()));
        init_data.add_vertex(Vec3f(b_max.x(), b_max.y() - size.y(), b_max.z()));
        init_data.add_vertex(Vec3f(b_max.x(), b_max.y(), b_max.z()));
        init_data.add_vertex(Vec3f(b_max.x(), b_max.y(), b_max.z() - size.z()));

        init_data.add_vertex(Vec3f(b_min.x(), b_max.y(), b_max.z()));
        init_data.add_vertex(Vec3f(b_min.x() + size.x(), b_max.y(), b_max.z()));
        init_data.add_vertex(Vec3f(b_min.x(), b_max.y(), b_max.z()));
        init_data.add_vertex(Vec3f(b_min.x(), b_max.y() - size.y(), b_max.z()));
        init_data.add_vertex(Vec3f(b_min.x(), b_max.y(), b_max.z()));
        init_data.add_vertex(Vec3f(b_min.x(), b_max.y(), b_max.z() - size.z()));

        // indices
        for (unsigned int i = 0; i < 48; ++i) {
            init_data.add_index(i);
        }

        m_box.init_from(std::move(init_data));
    }

    glsafe(::glEnable(GL_DEPTH_TEST));

    glsafe(::glLineWidth(2.0f * m_scale_factor));

    GLShaderProgram* shader = wxGetApp().get_shader("flat");
    if (shader == nullptr)
        return;

    shader->start_using();
#if ENABLE_GL_SHADERS_ATTRIBUTES
    const Camera& camera = wxGetApp().plater()->get_camera();
    shader->set_uniform("view_model_matrix", camera.get_view_matrix());
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());
#endif // ENABLE_GL_SHADERS_ATTRIBUTES
    m_box.set_color(to_rgba(color));
    m_box.render();
    shader->stop_using();
#else
    ::glBegin(GL_LINES);

    ::glVertex3f(b_min(0), b_min(1), b_min(2)); ::glVertex3f(b_min(0) + size(0), b_min(1), b_min(2));
    ::glVertex3f(b_min(0), b_min(1), b_min(2)); ::glVertex3f(b_min(0), b_min(1) + size(1), b_min(2));
    ::glVertex3f(b_min(0), b_min(1), b_min(2)); ::glVertex3f(b_min(0), b_min(1), b_min(2) + size(2));

    ::glVertex3f(b_max(0), b_min(1), b_min(2)); ::glVertex3f(b_max(0) - size(0), b_min(1), b_min(2));
    ::glVertex3f(b_max(0), b_min(1), b_min(2)); ::glVertex3f(b_max(0), b_min(1) + size(1), b_min(2));
    ::glVertex3f(b_max(0), b_min(1), b_min(2)); ::glVertex3f(b_max(0), b_min(1), b_min(2) + size(2));

    ::glVertex3f(b_max(0), b_max(1), b_min(2)); ::glVertex3f(b_max(0) - size(0), b_max(1), b_min(2));
    ::glVertex3f(b_max(0), b_max(1), b_min(2)); ::glVertex3f(b_max(0), b_max(1) - size(1), b_min(2));
    ::glVertex3f(b_max(0), b_max(1), b_min(2)); ::glVertex3f(b_max(0), b_max(1), b_min(2) + size(2));

    ::glVertex3f(b_min(0), b_max(1), b_min(2)); ::glVertex3f(b_min(0) + size(0), b_max(1), b_min(2));
    ::glVertex3f(b_min(0), b_max(1), b_min(2)); ::glVertex3f(b_min(0), b_max(1) - size(1), b_min(2));
    ::glVertex3f(b_min(0), b_max(1), b_min(2)); ::glVertex3f(b_min(0), b_max(1), b_min(2) + size(2));

    ::glVertex3f(b_min(0), b_min(1), b_max(2)); ::glVertex3f(b_min(0) + size(0), b_min(1), b_max(2));
    ::glVertex3f(b_min(0), b_min(1), b_max(2)); ::glVertex3f(b_min(0), b_min(1) + size(1), b_max(2));
    ::glVertex3f(b_min(0), b_min(1), b_max(2)); ::glVertex3f(b_min(0), b_min(1), b_max(2) - size(2));

    ::glVertex3f(b_max(0), b_min(1), b_max(2)); ::glVertex3f(b_max(0) - size(0), b_min(1), b_max(2));
    ::glVertex3f(b_max(0), b_min(1), b_max(2)); ::glVertex3f(b_max(0), b_min(1) + size(1), b_max(2));
    ::glVertex3f(b_max(0), b_min(1), b_max(2)); ::glVertex3f(b_max(0), b_min(1), b_max(2) - size(2));

    ::glVertex3f(b_max(0), b_max(1), b_max(2)); ::glVertex3f(b_max(0) - size(0), b_max(1), b_max(2));
    ::glVertex3f(b_max(0), b_max(1), b_max(2)); ::glVertex3f(b_max(0), b_max(1) - size(1), b_max(2));
    ::glVertex3f(b_max(0), b_max(1), b_max(2)); ::glVertex3f(b_max(0), b_max(1), b_max(2) - size(2));

    ::glVertex3f(b_min(0), b_max(1), b_max(2)); ::glVertex3f(b_min(0) + size(0), b_max(1), b_max(2));
    ::glVertex3f(b_min(0), b_max(1), b_max(2)); ::glVertex3f(b_min(0), b_max(1) - size(1), b_max(2));
    ::glVertex3f(b_min(0), b_max(1), b_max(2)); ::glVertex3f(b_min(0), b_max(1), b_max(2) - size(2));

    glsafe(::glEnd());
#endif // ENABLE_LEGACY_OPENGL_REMOVAL
}

static ColorRGBA get_color(Axis axis)
{
    return AXES_COLOR[axis];
}

#if ENABLE_GL_SHADERS_ATTRIBUTES
void Selection::render_sidebar_position_hints(const std::string& sidebar_field, GLShaderProgram& shader, const Transform3d& matrix)
#else
void Selection::render_sidebar_position_hints(const std::string& sidebar_field)
#endif // ENABLE_GL_SHADERS_ATTRIBUTES
{
#if ENABLE_LEGACY_OPENGL_REMOVAL
#if ENABLE_GL_SHADERS_ATTRIBUTES
    const Camera& camera = wxGetApp().plater()->get_camera();
    const Transform3d view_matrix = camera.get_view_matrix() * matrix;
    shader.set_uniform("projection_matrix", camera.get_projection_matrix());
#endif // ENABLE_GL_SHADERS_ATTRIBUTES

    if (boost::ends_with(sidebar_field, "x")) {
#if ENABLE_GL_SHADERS_ATTRIBUTES
        const Transform3d view_model_matrix = view_matrix * Geometry::assemble_transform(Vec3d::Zero(), -0.5 * PI * Vec3d::UnitZ());
        shader.set_uniform("view_model_matrix", view_model_matrix);
        shader.set_uniform("normal_matrix", (Matrix3d)view_model_matrix.matrix().block(0, 0, 3, 3).inverse().transpose());
#else
        glsafe(::glRotated(-90.0, 0.0, 0.0, 1.0));
#endif // ENABLE_GL_SHADERS_ATTRIBUTES
        m_arrow.set_color(get_color(X));
        m_arrow.render();
    }
    else if (boost::ends_with(sidebar_field, "y")) {
#if ENABLE_GL_SHADERS_ATTRIBUTES
        shader.set_uniform("view_model_matrix", view_matrix);
        shader.set_uniform("normal_matrix", (Matrix3d)view_matrix.matrix().block(0, 0, 3, 3).inverse().transpose());
#endif // ENABLE_GL_SHADERS_ATTRIBUTES
        m_arrow.set_color(get_color(Y));
        m_arrow.render();
    }
    else if (boost::ends_with(sidebar_field, "z")) {
#if ENABLE_GL_SHADERS_ATTRIBUTES
        const Transform3d view_model_matrix = view_matrix * Geometry::assemble_transform(Vec3d::Zero(), 0.5 * PI * Vec3d::UnitX());
        shader.set_uniform("view_model_matrix", view_model_matrix);
        shader.set_uniform("normal_matrix", (Matrix3d)view_model_matrix.matrix().block(0, 0, 3, 3).inverse().transpose());
#else
        glsafe(::glRotated(90.0, 1.0, 0.0, 0.0));
#endif // ENABLE_GL_SHADERS_ATTRIBUTES
        m_arrow.set_color(get_color(Z));
        m_arrow.render();
    }
#else
    if (boost::ends_with(sidebar_field, "x")) {
        glsafe(::glRotated(-90.0, 0.0, 0.0, 1.0));
        m_arrow.set_color(-1, get_color(X));
        m_arrow.render();
    }
    else if (boost::ends_with(sidebar_field, "y")) {
        m_arrow.set_color(-1, get_color(Y));
        m_arrow.render();
    }
    else if (boost::ends_with(sidebar_field, "z")) {
        glsafe(::glRotated(90.0, 1.0, 0.0, 0.0));
        m_arrow.set_color(-1, get_color(Z));
        m_arrow.render();
    }
#endif // ENABLE_LEGACY_OPENGL_REMOVAL
}

#if ENABLE_GL_SHADERS_ATTRIBUTES
void Selection::render_sidebar_rotation_hints(const std::string& sidebar_field, GLShaderProgram& shader, const Transform3d& matrix)
#else
void Selection::render_sidebar_rotation_hints(const std::string& sidebar_field)
#endif // ENABLE_GL_SHADERS_ATTRIBUTES
{
#if ENABLE_LEGACY_OPENGL_REMOVAL
#if ENABLE_GL_SHADERS_ATTRIBUTES
    auto render_sidebar_rotation_hint = [this](GLShaderProgram& shader, const Transform3d& matrix) {
        Transform3d view_model_matrix = matrix;
        shader.set_uniform("view_model_matrix", view_model_matrix);
        shader.set_uniform("normal_matrix", (Matrix3d)view_model_matrix.matrix().block(0, 0, 3, 3).inverse().transpose());
        m_curved_arrow.render();
        view_model_matrix = matrix * Geometry::assemble_transform(Vec3d::Zero(), PI * Vec3d::UnitZ());
        shader.set_uniform("view_model_matrix", view_model_matrix);
        shader.set_uniform("normal_matrix", (Matrix3d)view_model_matrix.matrix().block(0, 0, 3, 3).inverse().transpose());
        m_curved_arrow.render();
    };

    const Camera& camera = wxGetApp().plater()->get_camera();
    const Transform3d view_matrix = camera.get_view_matrix() * matrix;
    shader.set_uniform("projection_matrix", camera.get_projection_matrix());
#else
    auto render_sidebar_rotation_hint = [this]() {
        m_curved_arrow.render();
        glsafe(::glRotated(180.0, 0.0, 0.0, 1.0));
        m_curved_arrow.render();
    };
#endif // ENABLE_GL_SHADERS_ATTRIBUTES

    if (boost::ends_with(sidebar_field, "x")) {
#if !ENABLE_GL_SHADERS_ATTRIBUTES
        glsafe(::glRotated(90.0, 0.0, 1.0, 0.0));
#endif // !ENABLE_GL_SHADERS_ATTRIBUTES
        m_curved_arrow.set_color(get_color(X));
#if ENABLE_GL_SHADERS_ATTRIBUTES
        render_sidebar_rotation_hint(shader, view_matrix * Geometry::assemble_transform(Vec3d::Zero(), 0.5 * PI * Vec3d::UnitY()));
#else
        render_sidebar_rotation_hint();
#endif // ENABLE_GL_SHADERS_ATTRIBUTES
    }
    else if (boost::ends_with(sidebar_field, "y")) {
#if !ENABLE_GL_SHADERS_ATTRIBUTES
        glsafe(::glRotated(-90.0, 1.0, 0.0, 0.0));
#endif // !ENABLE_GL_SHADERS_ATTRIBUTES
        m_curved_arrow.set_color(get_color(Y));
#if ENABLE_GL_SHADERS_ATTRIBUTES
        render_sidebar_rotation_hint(shader, view_matrix * Geometry::assemble_transform(Vec3d::Zero(), -0.5 * PI * Vec3d::UnitX()));
#else
        render_sidebar_rotation_hint();
#endif // ENABLE_GL_SHADERS_ATTRIBUTES
    }
    else if (boost::ends_with(sidebar_field, "z")) {
        m_curved_arrow.set_color(get_color(Z));
#if ENABLE_GL_SHADERS_ATTRIBUTES
        render_sidebar_rotation_hint(shader, view_matrix);
#else
        render_sidebar_rotation_hint();
#endif // ENABLE_GL_SHADERS_ATTRIBUTES
    }
#else
    auto render_sidebar_rotation_hint = [this]() {
        m_curved_arrow.render();
        glsafe(::glRotated(180.0, 0.0, 0.0, 1.0));
        m_curved_arrow.render();
    };

    if (boost::ends_with(sidebar_field, "x")) {
        glsafe(::glRotated(90.0, 0.0, 1.0, 0.0));
        m_curved_arrow.set_color(-1, get_color(X));
        render_sidebar_rotation_hint();
    }
    else if (boost::ends_with(sidebar_field, "y")) {
        glsafe(::glRotated(-90.0, 1.0, 0.0, 0.0));
        m_curved_arrow.set_color(-1, get_color(Y));
        render_sidebar_rotation_hint();
    }
    else if (boost::ends_with(sidebar_field, "z")) {
        m_curved_arrow.set_color(-1, get_color(Z));
        render_sidebar_rotation_hint();
    }
#endif // ENABLE_LEGACY_OPENGL_REMOVAL
}

#if ENABLE_GL_SHADERS_ATTRIBUTES
void Selection::render_sidebar_scale_hints(const std::string& sidebar_field, GLShaderProgram& shader, const Transform3d& matrix)
#else
void Selection::render_sidebar_scale_hints(const std::string& sidebar_field)
#endif // ENABLE_GL_SHADERS_ATTRIBUTES
{
    const bool uniform_scale = requires_uniform_scale() || wxGetApp().obj_manipul()->get_uniform_scaling();

#if ENABLE_GL_SHADERS_ATTRIBUTES
    auto render_sidebar_scale_hint = [this, uniform_scale](Axis axis, GLShaderProgram& shader, const Transform3d& matrix) {
#else
    auto render_sidebar_scale_hint = [this, uniform_scale](Axis axis) {
#endif // ENABLE_GL_SHADERS_ATTRIBUTES
#if ENABLE_LEGACY_OPENGL_REMOVAL
        m_arrow.set_color(uniform_scale ? UNIFORM_SCALE_COLOR : get_color(axis));
#else
        m_arrow.set_color(-1, uniform_scale ? UNIFORM_SCALE_COLOR : get_color(axis));
#endif // ENABLE_LEGACY_OPENGL_REMOVAL

#if ENABLE_GL_SHADERS_ATTRIBUTES
        Transform3d view_model_matrix = matrix * Geometry::assemble_transform(5.0 * Vec3d::UnitY());
        shader.set_uniform("view_model_matrix", view_model_matrix);
        shader.set_uniform("normal_matrix", (Matrix3d)view_model_matrix.matrix().block(0, 0, 3, 3).inverse().transpose());
#else
        GLShaderProgram* shader = wxGetApp().get_current_shader();
        if (shader != nullptr)
            shader->set_uniform("emission_factor", 0.0f);

        glsafe(::glTranslated(0.0, 5.0, 0.0));
#endif // ENABLE_GL_SHADERS_ATTRIBUTES
        m_arrow.render();

#if ENABLE_GL_SHADERS_ATTRIBUTES
        view_model_matrix = matrix * Geometry::assemble_transform(-5.0 * Vec3d::UnitY(), PI * Vec3d::UnitZ());
        shader.set_uniform("view_model_matrix", view_model_matrix);
        shader.set_uniform("normal_matrix", (Matrix3d)view_model_matrix.matrix().block(0, 0, 3, 3).inverse().transpose());
#else
        glsafe(::glTranslated(0.0, -10.0, 0.0));
        glsafe(::glRotated(180.0, 0.0, 0.0, 1.0));
#endif // ENABLE_GL_SHADERS_ATTRIBUTES
        m_arrow.render();
    };

#if ENABLE_GL_SHADERS_ATTRIBUTES
    const Camera& camera = wxGetApp().plater()->get_camera();
    const Transform3d view_matrix = camera.get_view_matrix() * matrix;
    shader.set_uniform("projection_matrix", camera.get_projection_matrix());
#endif // ENABLE_GL_SHADERS_ATTRIBUTES

    if (boost::ends_with(sidebar_field, "x") || uniform_scale) {
#if ENABLE_GL_SHADERS_ATTRIBUTES
        render_sidebar_scale_hint(X, shader, view_matrix * Geometry::assemble_transform(Vec3d::Zero(), -0.5 * PI * Vec3d::UnitZ()));
#else
        glsafe(::glPushMatrix());
        glsafe(::glRotated(-90.0, 0.0, 0.0, 1.0));
        render_sidebar_scale_hint(X);
        glsafe(::glPopMatrix());
#endif // ENABLE_GL_SHADERS_ATTRIBUTES
    }

    if (boost::ends_with(sidebar_field, "y") || uniform_scale) {
#if ENABLE_GL_SHADERS_ATTRIBUTES
        render_sidebar_scale_hint(Y, shader, view_matrix);
#else
        glsafe(::glPushMatrix());
        render_sidebar_scale_hint(Y);
        glsafe(::glPopMatrix());
#endif // ENABLE_GL_SHADERS_ATTRIBUTES
    }

    if (boost::ends_with(sidebar_field, "z") || uniform_scale) {
#if ENABLE_GL_SHADERS_ATTRIBUTES
        render_sidebar_scale_hint(Z, shader, view_matrix * Geometry::assemble_transform(Vec3d::Zero(), 0.5 * PI * Vec3d::UnitX()));
#else
        glsafe(::glPushMatrix());
        glsafe(::glRotated(90.0, 1.0, 0.0, 0.0));
        render_sidebar_scale_hint(Z);
        glsafe(::glPopMatrix());
#endif // ENABLE_GL_SHADERS_ATTRIBUTES
    }
}

#if ENABLE_GL_SHADERS_ATTRIBUTES
void Selection::render_sidebar_layers_hints(const std::string& sidebar_field, GLShaderProgram& shader)
#else
void Selection::render_sidebar_layers_hints(const std::string& sidebar_field)
#endif // ENABLE_GL_SHADERS_ATTRIBUTES
{
    static const float Margin = 10.0f;

    std::string field = sidebar_field;

    // extract max_z
    std::string::size_type pos = field.rfind("_");
    if (pos == std::string::npos)
        return;

    const float max_z = float(string_to_double_decimal_point(field.substr(pos + 1)));

    // extract min_z
    field = field.substr(0, pos);
    pos = field.rfind("_");
    if (pos == std::string::npos)
        return;

    const float min_z = float(string_to_double_decimal_point(field.substr(pos + 1)));

    // extract type
    field = field.substr(0, pos);
    pos = field.rfind("_");
    if (pos == std::string::npos)
        return;

    const int type = std::stoi(field.substr(pos + 1));

    const BoundingBoxf3& box = get_bounding_box();

#if !ENABLE_LEGACY_OPENGL_REMOVAL
    const float min_x = float(box.min.x()) - Margin;
    const float max_x = float(box.max.x()) + Margin;
    const float min_y = float(box.min.y()) - Margin;
    const float max_y = float(box.max.y()) + Margin;
#endif // !ENABLE_LEGACY_OPENGL_REMOVAL

    // view dependend order of rendering to keep correct transparency
    const bool camera_on_top = wxGetApp().plater()->get_camera().is_looking_downward();
    const float z1 = camera_on_top ? min_z : max_z;
    const float z2 = camera_on_top ? max_z : min_z;

#if ENABLE_LEGACY_OPENGL_REMOVAL
    const Vec3f p1 = { float(box.min.x()) - Margin, float(box.min.y()) - Margin, z1 };
    const Vec3f p2 = { float(box.max.x()) + Margin, float(box.max.y()) + Margin, z2 };
#endif // ENABLE_LEGACY_OPENGL_REMOVAL

    glsafe(::glEnable(GL_DEPTH_TEST));
    glsafe(::glDisable(GL_CULL_FACE));
    glsafe(::glEnable(GL_BLEND));
    glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));

#if ENABLE_LEGACY_OPENGL_REMOVAL
    if (!m_planes.models[0].is_initialized() || !is_approx(m_planes.check_points[0], p1)) {
        m_planes.check_points[0] = p1;
        m_planes.models[0].reset();

        GLModel::Geometry init_data;
        init_data.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3 };
        init_data.reserve_vertices(4);
        init_data.reserve_indices(6);

        // vertices
        init_data.add_vertex(Vec3f(p1.x(), p1.y(), z1));
        init_data.add_vertex(Vec3f(p2.x(), p1.y(), z1));
        init_data.add_vertex(Vec3f(p2.x(), p2.y(), z1));
        init_data.add_vertex(Vec3f(p1.x(), p2.y(), z1));

        // indices
        init_data.add_triangle(0, 1, 2);
        init_data.add_triangle(2, 3, 0);

        m_planes.models[0].init_from(std::move(init_data));
    }

    if (!m_planes.models[1].is_initialized() || !is_approx(m_planes.check_points[1], p2)) {
        m_planes.check_points[1] = p2;
        m_planes.models[1].reset();

        GLModel::Geometry init_data;
        init_data.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3 };
        init_data.reserve_vertices(4);
        init_data.reserve_indices(6);

        // vertices
        init_data.add_vertex(Vec3f(p1.x(), p1.y(), z2));
        init_data.add_vertex(Vec3f(p2.x(), p1.y(), z2));
        init_data.add_vertex(Vec3f(p2.x(), p2.y(), z2));
        init_data.add_vertex(Vec3f(p1.x(), p2.y(), z2));

        // indices
        init_data.add_triangle(0, 1, 2);
        init_data.add_triangle(2, 3, 0);

        m_planes.models[1].init_from(std::move(init_data));
    }

#if ENABLE_GL_SHADERS_ATTRIBUTES
    const Camera& camera = wxGetApp().plater()->get_camera();
    shader.set_uniform("view_model_matrix", camera.get_view_matrix());
    shader.set_uniform("projection_matrix", camera.get_projection_matrix());
#endif // ENABLE_GL_SHADERS_ATTRIBUTES

    m_planes.models[0].set_color((camera_on_top && type == 1) || (!camera_on_top && type == 2) ? SOLID_PLANE_COLOR : TRANSPARENT_PLANE_COLOR);
    m_planes.models[0].render();
    m_planes.models[1].set_color((camera_on_top && type == 2) || (!camera_on_top && type == 1) ? SOLID_PLANE_COLOR : TRANSPARENT_PLANE_COLOR);
    m_planes.models[1].render();
#else
    ::glBegin(GL_QUADS);
    ::glColor4fv((camera_on_top && type == 1) || (!camera_on_top && type == 2) ? SOLID_PLANE_COLOR.data() : TRANSPARENT_PLANE_COLOR.data());
    ::glVertex3f(min_x, min_y, z1);
    ::glVertex3f(max_x, min_y, z1);
    ::glVertex3f(max_x, max_y, z1);
    ::glVertex3f(min_x, max_y, z1);
    glsafe(::glEnd());

    ::glBegin(GL_QUADS);
    ::glColor4fv((camera_on_top && type == 2) || (!camera_on_top && type == 1) ? SOLID_PLANE_COLOR.data() : TRANSPARENT_PLANE_COLOR.data());
    ::glVertex3f(min_x, min_y, z2);
    ::glVertex3f(max_x, min_y, z2);
    ::glVertex3f(max_x, max_y, z2);
    ::glVertex3f(min_x, max_y, z2);
    glsafe(::glEnd());
#endif // ENABLE_LEGACY_OPENGL_REMOVAL

    glsafe(::glEnable(GL_CULL_FACE));
    glsafe(::glDisable(GL_BLEND));
}

#ifndef NDEBUG
static bool is_rotation_xy_synchronized(const Vec3d &rot_xyz_from, const Vec3d &rot_xyz_to)
{
    const Eigen::AngleAxisd angle_axis(Geometry::rotation_xyz_diff(rot_xyz_from, rot_xyz_to));
    const Vec3d  axis = angle_axis.axis();
    const double angle = angle_axis.angle();
    if (std::abs(angle) < 1e-8)
        return true;
    assert(std::abs(axis.x()) < 1e-8);
    assert(std::abs(axis.y()) < 1e-8);
    assert(std::abs(std::abs(axis.z()) - 1.) < 1e-8);
    return std::abs(axis.x()) < 1e-8 && std::abs(axis.y()) < 1e-8 && std::abs(std::abs(axis.z()) - 1.) < 1e-8;
}

static void verify_instances_rotation_synchronized(const Model &model, const GLVolumePtrs &volumes)
{
    for (int idx_object = 0; idx_object < int(model.objects.size()); ++idx_object) {
        int idx_volume_first = -1;
        for (int i = 0; i < (int)volumes.size(); ++i) {
            if (volumes[i]->object_idx() == idx_object) {
                idx_volume_first = i;
                break;
            }
        }
        assert(idx_volume_first != -1); // object without instances?
        if (idx_volume_first == -1)
            continue;
        const Vec3d &rotation0 = volumes[idx_volume_first]->get_instance_rotation();
        for (int i = idx_volume_first + 1; i < (int)volumes.size(); ++i)
            if (volumes[i]->object_idx() == idx_object) {
                const Vec3d &rotation = volumes[i]->get_instance_rotation();
                assert(is_rotation_xy_synchronized(rotation, rotation0));
            }
    }
}
#endif /* NDEBUG */

void Selection::synchronize_unselected_instances(SyncRotationType sync_rotation_type)
{
    std::set<unsigned int> done;  // prevent processing volumes twice
    done.insert(m_list.begin(), m_list.end());

    for (unsigned int i : m_list) {
        if (done.size() == m_volumes->size())
            break;

        const GLVolume* volume = (*m_volumes)[i];
#if ENABLE_WIPETOWER_OBJECTID_1000_REMOVAL
        if (volume->is_wipe_tower)
            continue;

        const int object_idx = volume->object_idx();
#else
        const int object_idx = volume->object_idx();
        if (object_idx >= 1000)
            continue;
#endif // ENABLE_WIPETOWER_OBJECTID_1000_REMOVAL

        const int instance_idx = volume->instance_idx();
        const Vec3d& rotation = volume->get_instance_rotation();
        const Vec3d& scaling_factor = volume->get_instance_scaling_factor();
        const Vec3d& mirror = volume->get_instance_mirror();

        // Process unselected instances.
        for (unsigned int j = 0; j < (unsigned int)m_volumes->size(); ++j) {
            if (done.size() == m_volumes->size())
                break;

            if (done.find(j) != done.end())
                continue;

            GLVolume* v = (*m_volumes)[j];
            if (v->object_idx() != object_idx || v->instance_idx() == instance_idx)
                continue;

            assert(is_rotation_xy_synchronized(m_cache.volumes_data[i].get_instance_rotation(), m_cache.volumes_data[j].get_instance_rotation()));
            switch (sync_rotation_type) {
            case SyncRotationType::NONE: {
                // z only rotation -> synch instance z
                // The X,Y rotations should be synchronized from start to end of the rotation.
                assert(is_rotation_xy_synchronized(rotation, v->get_instance_rotation()));
                if (wxGetApp().preset_bundle->printers.get_edited_preset().printer_technology() != ptSLA)
                    v->set_instance_offset(Z, volume->get_instance_offset().z());
                break;
            }
            case SyncRotationType::GENERAL: {
                // generic rotation -> update instance z with the delta of the rotation.
                const double z_diff = Geometry::rotation_diff_z(m_cache.volumes_data[i].get_instance_rotation(), m_cache.volumes_data[j].get_instance_rotation());
                v->set_instance_rotation({ rotation.x(), rotation.y(), rotation.z() + z_diff });
                break;
            }
#if ENABLE_WORLD_COORDINATE
            case SyncRotationType::FULL: {
                // generic rotation -> update instance z with the delta of the rotation.
                const Eigen::AngleAxisd angle_axis(Geometry::rotation_xyz_diff(rotation, m_cache.volumes_data[j].get_instance_rotation()));
                const Vec3d& axis = angle_axis.axis();
                const double z_diff = (std::abs(axis.x()) > EPSILON || std::abs(axis.y()) > EPSILON) ?
                    angle_axis.angle() * axis.z() : Geometry::rotation_diff_z(rotation, m_cache.volumes_data[j].get_instance_rotation());

                v->set_instance_rotation({ rotation.x(), rotation.y(), rotation.z() + z_diff });
                break;
            }
#endif // ENABLE_WORLD_COORDINATE
            }

            v->set_instance_scaling_factor(scaling_factor);
            v->set_instance_mirror(mirror);

            done.insert(j);
        }
    }

#ifndef NDEBUG
    verify_instances_rotation_synchronized(*m_model, *m_volumes);
#endif /* NDEBUG */
}

void Selection::synchronize_unselected_volumes()
{
    for (unsigned int i : m_list) {
        const GLVolume* volume = (*m_volumes)[i];
#if ENABLE_WIPETOWER_OBJECTID_1000_REMOVAL
        if (volume->is_wipe_tower)
            continue;

        const int object_idx = volume->object_idx();
#else
        const int object_idx = volume->object_idx();
        if (object_idx >= 1000)
            continue;
#endif // ENABLE_WIPETOWER_OBJECTID_1000_REMOVAL

        const int volume_idx = volume->volume_idx();
        const Vec3d& offset = volume->get_volume_offset();
        const Vec3d& rotation = volume->get_volume_rotation();
        const Vec3d& scaling_factor = volume->get_volume_scaling_factor();
        const Vec3d& mirror = volume->get_volume_mirror();

        // Process unselected volumes.
        for (unsigned int j = 0; j < (unsigned int)m_volumes->size(); ++j) {
            if (j == i)
                continue;

            GLVolume* v = (*m_volumes)[j];
            if (v->object_idx() != object_idx || v->volume_idx() != volume_idx)
                continue;

            v->set_volume_offset(offset);
            v->set_volume_rotation(rotation);
            v->set_volume_scaling_factor(scaling_factor);
            v->set_volume_mirror(mirror);
        }
    }
}

void Selection::ensure_on_bed()
{
    typedef std::map<std::pair<int, int>, double> InstancesToZMap;
    InstancesToZMap instances_min_z;

    for (size_t i = 0; i < m_volumes->size(); ++i) {
        GLVolume* volume = (*m_volumes)[i];
        if (!volume->is_wipe_tower && !volume->is_modifier && 
            std::find(m_cache.sinking_volumes.begin(), m_cache.sinking_volumes.end(), i) == m_cache.sinking_volumes.end()) {
            const double min_z = volume->transformed_convex_hull_bounding_box().min.z();
            std::pair<int, int> instance = std::make_pair(volume->object_idx(), volume->instance_idx());
            InstancesToZMap::iterator it = instances_min_z.find(instance);
            if (it == instances_min_z.end())
                it = instances_min_z.insert(InstancesToZMap::value_type(instance, DBL_MAX)).first;

            it->second = std::min(it->second, min_z);
        }
    }

    for (GLVolume* volume : *m_volumes) {
        std::pair<int, int> instance = std::make_pair(volume->object_idx(), volume->instance_idx());
        InstancesToZMap::iterator it = instances_min_z.find(instance);
        if (it != instances_min_z.end())
            volume->set_instance_offset(Z, volume->get_instance_offset(Z) - it->second);
    }
}

void Selection::ensure_not_below_bed()
{
    typedef std::map<std::pair<int, int>, double> InstancesToZMap;
    InstancesToZMap instances_max_z;

    for (size_t i = 0; i < m_volumes->size(); ++i) {
        GLVolume* volume = (*m_volumes)[i];
        if (!volume->is_wipe_tower && !volume->is_modifier) {
            const double max_z = volume->transformed_convex_hull_bounding_box().max.z();
            const std::pair<int, int> instance = std::make_pair(volume->object_idx(), volume->instance_idx());
            InstancesToZMap::iterator it = instances_max_z.find(instance);
            if (it == instances_max_z.end())
                it = instances_max_z.insert({ instance, -DBL_MAX }).first;

            it->second = std::max(it->second, max_z);
        }
    }

    if (is_any_volume()) {
        for (unsigned int i : m_list) {
            GLVolume& volume = *(*m_volumes)[i];
            const std::pair<int, int> instance = std::make_pair(volume.object_idx(), volume.instance_idx());
            InstancesToZMap::const_iterator it = instances_max_z.find(instance);
            const double z_shift = SINKING_MIN_Z_THRESHOLD - it->second;
            if (it != instances_max_z.end() && z_shift > 0.0)
                volume.set_volume_offset(Z, volume.get_volume_offset(Z) + z_shift);
        }
    }
    else {
        for (GLVolume* volume : *m_volumes) {
            const std::pair<int, int> instance = std::make_pair(volume->object_idx(), volume->instance_idx());
            InstancesToZMap::const_iterator it = instances_max_z.find(instance);
            if (it != instances_max_z.end() && it->second < SINKING_MIN_Z_THRESHOLD)
                volume->set_instance_offset(Z, volume->get_instance_offset(Z) + SINKING_MIN_Z_THRESHOLD - it->second);
        }
    }
}

bool Selection::is_from_fully_selected_instance(unsigned int volume_idx) const
{
    struct SameInstance
    {
        int obj_idx;
        int inst_idx;
        GLVolumePtrs& volumes;

        SameInstance(int obj_idx, int inst_idx, GLVolumePtrs& volumes) : obj_idx(obj_idx), inst_idx(inst_idx), volumes(volumes) {}
        bool operator () (unsigned int i) { return (volumes[i]->volume_idx() >= 0) && (volumes[i]->object_idx() == obj_idx) && (volumes[i]->instance_idx() == inst_idx); }
    };

    if ((unsigned int)m_volumes->size() <= volume_idx)
        return false;

    GLVolume* volume = (*m_volumes)[volume_idx];
    int object_idx = volume->object_idx();
    if ((int)m_model->objects.size() <= object_idx)
        return false;

    unsigned int count = (unsigned int)std::count_if(m_list.begin(), m_list.end(), SameInstance(object_idx, volume->instance_idx(), *m_volumes));
    return count == (unsigned int)m_model->objects[object_idx]->volumes.size();
}

void Selection::paste_volumes_from_clipboard()
{
#ifdef _DEBUG
    check_model_ids_validity(*m_model);
#endif /* _DEBUG */

    int dst_obj_idx = get_object_idx();
    if ((dst_obj_idx < 0) || ((int)m_model->objects.size() <= dst_obj_idx))
        return;

    ModelObject* dst_object = m_model->objects[dst_obj_idx];

    int dst_inst_idx = get_instance_idx();
    if ((dst_inst_idx < 0) || ((int)dst_object->instances.size() <= dst_inst_idx))
        return;

    ModelObject* src_object = m_clipboard.get_object(0);
    if (src_object != nullptr)
    {
        ModelInstance* dst_instance = dst_object->instances[dst_inst_idx];
        BoundingBoxf3 dst_instance_bb = dst_object->instance_bounding_box(dst_inst_idx);
        Transform3d src_matrix = src_object->instances[0]->get_transformation().get_matrix(true);
        Transform3d dst_matrix = dst_instance->get_transformation().get_matrix(true);
        bool from_same_object = (src_object->input_file == dst_object->input_file) && src_matrix.isApprox(dst_matrix);

        // used to keep relative position of multivolume selections when pasting from another object
        BoundingBoxf3 total_bb;

        ModelVolumePtrs volumes;
        for (ModelVolume* src_volume : src_object->volumes)
        {
            ModelVolume* dst_volume = dst_object->add_volume(*src_volume);
            dst_volume->set_new_unique_id();
            if (from_same_object)
            {
//                // if the volume comes from the same object, apply the offset in world system
//                double offset = wxGetApp().plater()->canvas3D()->get_size_proportional_to_max_bed_size(0.05);
//                dst_volume->translate(dst_matrix.inverse() * Vec3d(offset, offset, 0.0));
            }
            else
            {
                // if the volume comes from another object, apply the offset as done when adding modifiers
                // see ObjectList::load_generic_subobject()
                total_bb.merge(dst_volume->mesh().bounding_box().transformed(src_volume->get_matrix()));
            }

            volumes.push_back(dst_volume);
#ifdef _DEBUG
		    check_model_ids_validity(*m_model);
#endif /* _DEBUG */
        }

        // keeps relative position of multivolume selections
        if (!from_same_object)
        {
            for (ModelVolume* v : volumes)
            {
                v->set_offset((v->get_offset() - total_bb.center()) + dst_matrix.inverse() * (Vec3d(dst_instance_bb.max(0), dst_instance_bb.min(1), dst_instance_bb.min(2)) + 0.5 * total_bb.size() - dst_instance->get_transformation().get_offset()));
            }
        }

        wxGetApp().obj_list()->paste_volumes_into_list(dst_obj_idx, volumes);
    }

#ifdef _DEBUG
    check_model_ids_validity(*m_model);
#endif /* _DEBUG */
}

void Selection::paste_objects_from_clipboard()
{
#ifdef _DEBUG
    check_model_ids_validity(*m_model);
#endif /* _DEBUG */

    std::vector<size_t> object_idxs;
    const ModelObjectPtrs& src_objects = m_clipboard.get_objects();
    for (const ModelObject* src_object : src_objects)
    {
        ModelObject* dst_object = m_model->add_object(*src_object);
        double offset = wxGetApp().plater()->canvas3D()->get_size_proportional_to_max_bed_size(0.05);
        Vec3d displacement(offset, offset, 0.0);
        for (ModelInstance* inst : dst_object->instances)
        {
            inst->set_offset(inst->get_offset() + displacement);
        }

        object_idxs.push_back(m_model->objects.size() - 1);
#ifdef _DEBUG
	    check_model_ids_validity(*m_model);
#endif /* _DEBUG */
    }

    wxGetApp().obj_list()->paste_objects_into_list(object_idxs);

#ifdef _DEBUG
    check_model_ids_validity(*m_model);
#endif /* _DEBUG */
}

} // namespace GUI
} // namespace Slic3r
