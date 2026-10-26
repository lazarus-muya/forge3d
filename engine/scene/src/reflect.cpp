// SPDX-License-Identifier: Apache-2.0
#include "f3d/scene/reflect.hpp"

#include "f3d/core/reflect.hpp"
#include "f3d/scene/components.hpp"
#include "f3d/scene/layers.hpp"

#include <entt/meta/factory.hpp>

#include <algorithm>

namespace f3d::scene {
namespace {

using core::reflect::FieldMeta;

/// A value type's meta node (no name): the JSON layer recognises it by id.
void register_enum_values() {
    entt::meta_factory<LightType>{}
        .type("LightType")
        .data<LightType::Directional>("Directional")
        .data<LightType::Point>("Point")
        .data<LightType::Spot>("Spot");
    entt::meta_factory<CameraProjection>{}
        .type("CameraProjection")
        .data<CameraProjection::Perspective>("Perspective")
        .data<CameraProjection::Orthographic>("Orthographic");
}

void register_components() {
    entt::meta_factory<Name>{}.type("Name").data<&Name::value>("value").custom<FieldMeta>(
        FieldMeta{}.with_label("Name").with_tooltip("Display name in the Outliner"));

    entt::meta_factory<Transform>{}
        .type("Transform")
        .data<&Transform::translation>("translation")
        .custom<FieldMeta>(FieldMeta{}.with_label("Position").with_unit("m").with_range(-1.0e5, 1.0e5, 0.01).with_tooltip(
            "Where the object is, relative to its parent (X, Y up, Z)"))
        .data<&Transform::rotation>("rotation")
        .custom<FieldMeta>(FieldMeta{}.with_tooltip("Rotation about X, Y and Z, in degrees"))
        .data<&Transform::scale>("scale")
        .custom<FieldMeta>(FieldMeta{}.with_range(-100.0, 100.0, 0.01).with_tooltip(
            "Size multiplier per axis (1: original size)"));

    entt::meta_factory<MeshRenderer>{}
        .type("MeshRenderer")
        .data<&MeshRenderer::mesh>("mesh")
        .custom<FieldMeta>(FieldMeta{}.with_tooltip("The model this object draws"))
        .data<&MeshRenderer::visible>("visible")
        .custom<FieldMeta>(FieldMeta{}.with_tooltip("Draw the mesh (off: hidden but kept)"))
        .data<&MeshRenderer::texture>("texture")
        .custom<FieldMeta>(FieldMeta{}.with_tooltip("Base-colour texture override (drop a texture asset here)"))
        .data<&MeshRenderer::materials>("materials")
        .custom<FieldMeta>(FieldMeta{}.with_tooltip("Material per submesh (empty: the model's own)"));

    entt::meta_factory<Light>{}
        .type("Light")
        .data<&Light::type>("type")
        .custom<FieldMeta>(FieldMeta{}.with_tooltip(
            "Directional: parallel rays like the sun. Point: shines in every direction. Spot: a cone"))
        .data<&Light::color>("color")
        .custom<FieldMeta>(FieldMeta{}.with_label("Colour").with_color().with_tooltip("Light colour (white: neutral)"))
        .data<&Light::intensity>("intensity")
        .custom<FieldMeta>(FieldMeta{}.with_label("Brightness").with_range(0.0, 1.0e6, 1.0).with_tooltip(
            "Lux for a directional light (sun: about 100000), candela for point and spot lights"))
        .data<&Light::range>("range")
        .custom<FieldMeta>(FieldMeta{}.with_unit("m").with_range(0.0, 1.0e4, 0.1).with_tooltip(
            "Distance at which a point or spot light fades out completely"))
        .data<&Light::inner_cone>("inner_cone")
        .custom<FieldMeta>(FieldMeta{}.with_unit("rad").with_range(0.0, 1.5707, 0.01).with_tooltip(
            "Spot light: half-angle of the fully lit centre"))
        .data<&Light::outer_cone>("outer_cone")
        .custom<FieldMeta>(FieldMeta{}.with_unit("rad").with_range(0.0, 1.5707, 0.01).with_tooltip(
            "Spot light: half-angle where the light ends; the edge softens between inner and outer"))
        .data<&Light::cast_shadows>("cast_shadows")
        .custom<FieldMeta>(FieldMeta{}.with_tooltip("Objects block this light and cast shadows"))
        .data<&Light::volumetric_scattering>("volumetric_scattering")
        .custom<FieldMeta>(FieldMeta{}.with_label("Fog scattering").with_range(0.0, 20.0, 0.05).with_tooltip(
            "How strongly the light shows up as beams in volumetric fog (1: physically, 0: not at all)"));

    entt::meta_factory<HeightFog>{}
        .type("HeightFog")
        .data<&HeightFog::density>("density")
        .custom<FieldMeta>(FieldMeta{}.with_label("Thickness").with_unit("1/m").with_range(0.0, 0.5, 0.0001).with_tooltip(
            "Fog density at the entity's height; you see about 3 / density metres. "
            "0.0005: clear haze, 0.002: haze, 0.01: mist, 0.05: thick fog"))
        .data<&HeightFog::height_falloff>("height_falloff")
        .custom<FieldMeta>(FieldMeta{}.with_unit("1/m").with_range(0.0, 2.0, 0.001).with_tooltip(
            "How fast the fog thins upwards (0.02: half as dense every 35 m)"))
        .data<&HeightFog::albedo>("albedo")
        .custom<FieldMeta>(FieldMeta{}.with_label("Colour").with_color().with_range(0.0, 1.0, 0.01).with_tooltip(
            "How much light the fog reflects: below 1 is grey haze, 1 is white mist"))
        .data<&HeightFog::anisotropy>("anisotropy")
        .custom<FieldMeta>(FieldMeta{}.with_label("Sun glow").with_range(-0.9, 0.95, 0.01).with_tooltip(
            "Forward scattering: the bright halo when looking towards the sun (0: even in every direction)"))
        .data<&HeightFog::sky_lighting>("sky_lighting")
        .custom<FieldMeta>(FieldMeta{}.with_range(0.0, 4.0, 0.01).with_tooltip(
            "How much ambient sky light brightens the fog"))
        .data<&HeightFog::sun_lighting>("sun_lighting")
        .custom<FieldMeta>(FieldMeta{}.with_range(0.0, 4.0, 0.01).with_tooltip(
            "How much the sun (directional light) brightens the fog"))
        .data<&HeightFog::start_distance>("start_distance")
        .custom<FieldMeta>(FieldMeta{}.with_unit("m").with_range(0.0, 1.0e5, 1.0).with_tooltip(
            "Clear distance from the camera before the fog begins"))
        .data<&HeightFog::max_opacity>("max_opacity")
        .custom<FieldMeta>(FieldMeta{}.with_range(0.0, 1.0, 0.01).with_tooltip(
            "Most the fog can hide (1: distant things vanish completely)"))
        .data<&HeightFog::cutoff_distance>("cutoff_distance")
        .custom<FieldMeta>(FieldMeta{}.with_label("Sky distance").with_unit("m").with_range(0.0, 1.0e6, 10.0).with_tooltip(
            "How far away the sky counts as, for fogging it (0: 30 km)"))
        .data<&HeightFog::volumetric>("volumetric")
        .custom<FieldMeta>(FieldMeta{}.with_tooltip(
            "Shadowed fog near the camera: light shafts and light cones (costs more)"))
        .data<&HeightFog::volumetric_distance>("volumetric_distance")
        .custom<FieldMeta>(FieldMeta{}.with_unit("m").with_range(10.0, 2000.0, 1.0).with_tooltip(
            "How far the volumetric (light-shaft) fog reaches; beyond it the fog is unshadowed"))
        .data<&HeightFog::volumetric_density>("volumetric_density")
        .custom<FieldMeta>(FieldMeta{}.with_range(0.0, 10.0, 0.01).with_tooltip(
            "Thickness multiplier inside the volumetric part"))
        .data<&HeightFog::visible>("visible")
        .custom<FieldMeta>(FieldMeta{}.with_label("Enabled").with_tooltip("Apply this fog"));

    entt::meta_factory<FogVolume>{}
        .type("FogVolume")
        .data<&FogVolume::size>("size")
        .custom<FieldMeta>(FieldMeta{}.with_unit("m").with_range(0.1, 1.0e4, 0.1).with_tooltip(
            "Box size (width, height, depth), times the entity's scale"))
        .data<&FogVolume::density>("density")
        .custom<FieldMeta>(FieldMeta{}.with_label("Thickness").with_unit("1/m").with_range(0.0, 5.0, 0.001).with_tooltip(
            "Fog density at the bottom of the box (needs volumetric fog)"))
        .data<&FogVolume::albedo>("albedo")
        .custom<FieldMeta>(FieldMeta{}.with_label("Colour").with_color().with_range(0.0, 1.0, 0.01).with_tooltip(
            "How much light the fog reflects (1: white)"))
        .data<&FogVolume::height_falloff>("height_falloff")
        .custom<FieldMeta>(FieldMeta{}.with_unit("1/m").with_range(0.0, 10.0, 0.01).with_tooltip(
            "How fast the fog thins towards the top of the box (0: even)"))
        .data<&FogVolume::edge_fade>("edge_fade")
        .custom<FieldMeta>(FieldMeta{}.with_range(0.0, 1.0, 0.01).with_tooltip(
            "Softness of the sides: the fraction of the box over which the fog fades out"))
        .data<&FogVolume::emissive>("emissive")
        .custom<FieldMeta>(FieldMeta{}.with_label("Glow").with_color(true).with_range(0.0, 100.0, 0.01).with_tooltip(
            "Light the fog gives off itself, like glowing smoke (0: none)"))
        .data<&FogVolume::visible>("visible")
        .custom<FieldMeta>(FieldMeta{}.with_label("Enabled").with_tooltip("Apply this fog volume"));

    entt::meta_factory<PostProcess>{}
        .type("PostProcess")
        .data<&PostProcess::bloom_intensity>("bloom_intensity")
        .custom<FieldMeta>(FieldMeta{}.with_label("Bloom").with_range(0.0, 1.0, 0.005).with_tooltip(
            "Glow around bright things: the share of the image replaced by its blur"))
        .data<&PostProcess::bloom_radius>("bloom_radius")
        .custom<FieldMeta>(FieldMeta{}.with_range(0.0, 1.0, 0.01).with_tooltip("How far the glow spreads"))
        .data<&PostProcess::bloom_threshold>("bloom_threshold")
        .custom<FieldMeta>(FieldMeta{}.with_unit("EV").with_range(0.0, 10.0, 0.1).with_tooltip(
            "Only what is this much brighter than the exposure blooms (0: everything, like a real lens)"))
        .data<&PostProcess::vignette>("vignette")
        .custom<FieldMeta>(FieldMeta{}.with_range(0.0, 1.0, 0.01).with_tooltip("Darkening towards the corners"))
        .data<&PostProcess::temperature>("temperature")
        .custom<FieldMeta>(FieldMeta{}.with_label("White balance").with_unit("K").with_range(1500.0, 15000.0, 50.0)
                               .with_tooltip("The colour temperature that looks white: lower makes the image "
                                             "warmer, higher cooler (6500: neutral)"))
        .data<&PostProcess::tint>("tint")
        .custom<FieldMeta>(FieldMeta{}.with_range(-1.0, 1.0, 0.01).with_tooltip("Colour shift: -1 magenta, 1 green"))
        .data<&PostProcess::saturation>("saturation")
        .custom<FieldMeta>(FieldMeta{}.with_range(0.0, 2.0, 0.01).with_tooltip(
            "Colour strength (0: black and white, 1: unchanged)"))
        .data<&PostProcess::contrast>("contrast")
        .custom<FieldMeta>(FieldMeta{}.with_range(0.5, 2.0, 0.01).with_tooltip(
            "Difference between darks and lights (1: unchanged)"))
        .data<&PostProcess::film_grain>("film_grain")
        .custom<FieldMeta>(FieldMeta{}.with_range(0.0, 1.0, 0.01).with_tooltip("Film-like noise (0: none)"))
        .data<&PostProcess::visible>("visible")
        .custom<FieldMeta>(FieldMeta{}.with_label("Enabled").with_tooltip("Apply these effects"));

    entt::meta_factory<Sky>{}
        .type("Sky")
        .data<&Sky::time_of_day>("time_of_day")
        .custom<FieldMeta>(FieldMeta{}.with_unit("h").with_range(0.0, 24.0, 0.05).with_tooltip(
            "Local time (12: noon); moves the sun and the moon"))
        .data<&Sky::year>("year")
        .custom<FieldMeta>(FieldMeta{}.with_range(1900.0, 2200.0, 1.0).with_tooltip("Date, for the sun and moon paths"))
        .data<&Sky::month>("month")
        .custom<FieldMeta>(FieldMeta{}.with_range(1.0, 12.0, 1.0).with_tooltip(
            "Date, for the sun path (June: long summer days in the north)"))
        .data<&Sky::day>("day")
        .custom<FieldMeta>(FieldMeta{}.with_range(1.0, 31.0, 1.0).with_tooltip("Day of the month"))
        .data<&Sky::latitude>("latitude")
        .custom<FieldMeta>(FieldMeta{}.with_unit("deg").with_range(-90.0, 90.0, 0.1).with_tooltip(
            "Where on Earth (+ north): how high the sun climbs"))
        .data<&Sky::longitude>("longitude")
        .custom<FieldMeta>(FieldMeta{}.with_unit("deg").with_range(-180.0, 180.0, 0.1).with_tooltip(
            "Where on Earth (+ east): with the time zone, when the sun rises"))
        .data<&Sky::utc_offset>("utc_offset")
        .custom<FieldMeta>(FieldMeta{}.with_label("Time zone").with_unit("h").with_range(-14.0, 14.0, 0.25).with_tooltip(
            "Hours ahead of UTC that the time of day is in"))
        .data<&Sky::north_offset>("north_offset")
        .custom<FieldMeta>(FieldMeta{}.with_label("North direction").with_unit("deg").with_range(-180.0, 180.0, 1.0)
                               .with_tooltip("Turns the compass: north rotated clockwise from -Z (seen from above)"))
        .data<&Sky::sun_illuminance>("sun_illuminance")
        .custom<FieldMeta>(FieldMeta{}.with_label("Sun brightness").with_unit("lx").with_range(0.0, 200000.0, 100.0)
                               .with_tooltip("Sunlight above the atmosphere (real sun: about 128000)"))
        .data<&Sky::haze>("haze")
        .custom<FieldMeta>(FieldMeta{}.with_range(0.0, 20.0, 0.05).with_tooltip(
            "Dust and moisture in the air (1: normal; higher: hazier, whiter sky)"))
        .data<&Sky::ground_albedo>("ground_albedo")
        .custom<FieldMeta>(FieldMeta{}.with_label("Ground colour").with_color().with_tooltip(
            "Colour of the ground below the horizon; tints the sky light bounced back up"))
        .data<&Sky::moon>("moon")
        .custom<FieldMeta>(FieldMeta{}.with_tooltip("Moonlight when the sun is down"))
        .data<&Sky::shadows>("shadows")
        .custom<FieldMeta>(FieldMeta{}.with_tooltip("The sun (or moon) casts shadows"))
        .data<&Sky::exposure_compensation>("exposure_compensation")
        .custom<FieldMeta>(FieldMeta{}.with_label("Exposure").with_unit("EV").with_range(-10.0, 10.0, 0.1).with_tooltip(
            "Brighter (+) or darker (-) than the automatic exposure; each step doubles or halves"));

    entt::meta_factory<ReflectionProbe>{}
        .type("ReflectionProbe")
        .data<&ReflectionProbe::radius>("radius")
        .custom<FieldMeta>(FieldMeta{}.with_unit("m").with_range(0.1, 1.0e4, 0.1).with_tooltip(
            "Objects within this distance use this probe's reflections instead of the sky's"))
        .data<&ReflectionProbe::resolution>("resolution")
        .custom<FieldMeta>(FieldMeta{}.with_range(16.0, 512.0, 16.0).with_tooltip(
            "Reflection detail: cube face size in pixels (higher: sharper, slower to capture)"));

    entt::meta_factory<Layer>{}.type("Layer").data<&Layer::layer>("layer").custom<FieldMeta>(
        FieldMeta{}.with_tooltip("Layer (null: default)").with_read_only());

    entt::meta_factory<PrefabOverride>{}
        .type("PrefabOverride")
        .data<&PrefabOverride::node>("node")
        .data<&PrefabOverride::path>("path")
        .data<&PrefabOverride::value>("value");
    entt::meta_factory<std::vector<PrefabOverride>>{}.type("PrefabOverride[]");
    entt::meta_factory<PrefabInstance>{}
        .type("PrefabInstance")
        .data<&PrefabInstance::prefab>("prefab")
        .custom<FieldMeta>(FieldMeta{}.with_tooltip("The prefab this entity is an instance of").with_read_only())
        .data<&PrefabInstance::overrides>("overrides")
        .custom<FieldMeta>(FieldMeta{}.with_hidden());
    entt::meta_factory<PrefabLink>{}
        .type("PrefabLink")
        .data<&PrefabLink::node>("node")
        .custom<FieldMeta>(FieldMeta{}.with_hidden())
        .data<&PrefabLink::outer_node>("outer_node")
        .custom<FieldMeta>(FieldMeta{}.with_hidden());

    entt::meta_factory<LayerInfo>{}
        .type("LayerInfo")
        .data<&LayerInfo::id>("id")
        .data<&LayerInfo::name>("name")
        .data<&LayerInfo::visible>("visible")
        .data<&LayerInfo::locked>("locked");

    entt::meta_factory<CameraBookmark>{}
        .type("CameraBookmark")
        .data<&CameraBookmark::name>("name")
        .data<&CameraBookmark::target>("target")
        .data<&CameraBookmark::distance>("distance")
        .data<&CameraBookmark::yaw>("yaw")
        .data<&CameraBookmark::pitch>("pitch")
        .data<&CameraBookmark::orthographic>("orthographic");

    entt::meta_factory<Camera>{}
        .type("Camera")
        .data<&Camera::projection>("projection")
        .custom<FieldMeta>(FieldMeta{}.with_tooltip(
            "Perspective: far things look smaller. Orthographic: no perspective, for plans and elevations"))
        .data<&Camera::fov_y>("fov_y")
        .custom<FieldMeta>(FieldMeta{}.with_label("Field of view").with_unit("rad").with_range(0.01, 3.14, 0.01)
                               .with_tooltip("Vertical viewing angle (perspective); smaller: zoomed in"))
        .data<&Camera::ortho_height>("ortho_height")
        .custom<FieldMeta>(FieldMeta{}.with_label("View height").with_unit("m").with_range(0.01, 1.0e4, 0.1)
                               .with_tooltip("How many metres fit top to bottom (orthographic)"))
        .data<&Camera::near_plane>("near_plane")
        .custom<FieldMeta>(FieldMeta{}.with_label("Near clip").with_unit("m").with_tooltip(
            "Anything closer than this is not drawn"))
        .data<&Camera::far_plane>("far_plane")
        .custom<FieldMeta>(FieldMeta{}.with_label("Far clip").with_unit("m").with_tooltip(
            "Anything further than this is not drawn"))
        .data<&Camera::aspect_ratio>("aspect_ratio")
        .custom<FieldMeta>(FieldMeta{}.with_range(0.1, 10.0, 0.01).with_tooltip(
            "Frame width / height (1.78: 16:9) of the camera's preview and frustum"));
}

} // namespace

void register_types() {
    static const bool registered = [] {
        core::reflect::register_value_types();
        register_enum_values();
        register_components();
        register_component<MeshRenderer>("MeshRenderer");
        register_component<Light>("Light");
        register_component<Camera>("Camera");
        register_component<Sky>("Sky");
        register_component<ReflectionProbe>("ReflectionProbe");
        register_component<HeightFog>("HeightFog");
        register_component<FogVolume>("FogVolume");
        register_component<PostProcess>("PostProcess");
        register_component<Layer>("Layer", false);
        register_component<PrefabInstance>("PrefabInstance", false);
        register_component<PrefabLink>("PrefabLink", false);
        return true;
    }();
    (void)registered;
}

namespace {

std::vector<ComponentType>& types() {
    static std::vector<ComponentType> list;
    return list;
}

/// Visits the optional (non-Name/Transform) reflected components.
template <class F>
void for_each_optional(entt::registry& registry, entt::entity e, F&& fn) {
    for (const ComponentType& type : types()) {
        if (type.has(registry, e)) {
            fn(type.name.c_str(), type.get(registry, e));
        }
    }
}

const ComponentType* find_type(std::string_view name) {
    for (const ComponentType& type : types()) {
        if (type.name == name) {
            return &type;
        }
    }
    return nullptr;
}

} // namespace

void register_component_type(ComponentType type) {
    auto& list = types();
    for (ComponentType& existing : list) {
        if (existing.name == type.name) {
            existing = std::move(type);
            return;
        }
    }
    list.push_back(std::move(type));
}

const std::vector<ComponentType>& component_types() {
    return types();
}

void for_each_component(entt::registry& registry, entt::entity e, const std::function<void(entt::meta_any)>& fn) {
    if (auto* c = registry.try_get<Name>(e)) {
        fn(entt::forward_as_meta(*c));
    }
    if (auto* c = registry.try_get<Transform>(e)) {
        fn(entt::forward_as_meta(*c));
    }
    for_each_optional(registry, e, [&](const char*, entt::meta_any any) { fn(std::move(any)); });
}

entt::meta_any component_by_name(entt::registry& registry, entt::entity e, std::string_view type_name) {
    entt::meta_any found;
    for_each_component(registry, e, [&](entt::meta_any any) {
        const entt::meta_type type = any.type();
        if (type.name() != nullptr && type_name == type.name()) {
            found = std::move(any);
        }
    });
    return found;
}

std::vector<std::string_view> addable_components() {
    std::vector<std::string_view> names;
    for (const ComponentType& type : types()) {
        if (type.addable) {
            names.push_back(type.name);
        }
    }
    return names;
}

bool is_addable_component(std::string_view type_name) {
    const ComponentType* type = find_type(type_name);
    return type && type->addable;
}

bool remove_component(entt::registry& registry, entt::entity e, std::string_view type_name) {
    const ComponentType* type = find_type(type_name);
    return type && type->remove(registry, e);
}

bool add_component(entt::registry& registry, entt::entity e, std::string_view type_name) {
    const ComponentType* type = find_type(type_name);
    if (!type) {
        return false;
    }
    type->add(registry, e);
    return true;
}

} // namespace f3d::scene
