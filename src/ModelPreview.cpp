#include "ClientGeometry.h"
#include "ModelPreview.h"
#include "ModelResourceLimits.h"
#include "HandAnimationController.h"
#include "MolangPhysics.h"
#include "YsmUi.h"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

namespace ysm {
namespace {
glm::dvec3 triple(Json const& j,glm::dvec3 fallback={0,0,0}) {
    if(!j.is_array()||j.size()!=3)return fallback;
    for(int i=0;i<3;++i)if(j[i].is_number())fallback[i]=j[i].get<double>();return fallback;
}
Json const& field(Json const& object,char const* name) {
    static const Json empty;
    auto it=object.find(name);
    return it==object.end()?empty:*it;
}
glm::dvec3 vec(Triple const& a){return {a[0],a[1],a[2]};}
glm::dvec2 texturePair(Json const& value) {
    if (!value.is_array() || value.size() < 2 || !value[0].is_number() || !value[1].is_number()) return {};
    return {value[0].get<double>(), value[1].get<double>()};
}
glm::dvec3 sourcePoint(glm::dvec3 p){return {-p.x,p.y,p.z};}
glm::dvec3 sourceAngle(glm::dvec3 r){return {-r.x,-r.y,r.z};}
glm::dmat4 rotate(glm::dvec3 r){constexpr double rad=3.141592653589793/180;glm::dmat4 m(1);for(int i=2;i>=0;--i){if(r[i]==0)continue;glm::dvec3 axis(0);axis[i]=1;m=glm::rotate(m,r[i]*rad,axis);}return m;}
struct Face {glm::dvec3 p[4];glm::dvec2 uv[4];double light;BoneVisual visual;TextureImage const* image;double tw,th;};
void composite(TextureImage& target, TextureImage const& layer) {
    for (std::size_t y = 0; y < target.height; ++y) {
        auto layerY = y * layer.height / target.height;
        for (std::size_t x = 0; x < target.width; ++x) {
            auto layerX = x * layer.width / target.width;
            auto* out = &target.rgba[(y * target.width + x) * 4];
            auto const* in = &layer.rgba[(layerY * layer.width + layerX) * 4];
            auto inputAlpha = unsigned(in[3]) * 255;
            auto remainingAlpha = unsigned(out[3]) * (255 - in[3]);
            auto alpha = inputAlpha + remainingAlpha;
            if (!inputAlpha) continue;
            for (int channel = 0; channel < 3; ++channel)
                out[channel] = static_cast<unsigned char>((in[channel] * inputAlpha + out[channel] * remainingAlpha + alpha / 2) / alpha);
            out[3] = static_cast<unsigned char>((alpha + 127) / 255);
        }
    }
}
}
Json PreviewShoulders::readJson(std::string_view bytes) {
    if (bytes.size() > MaxModelResourceFile) return {};
    return Json::parse(bytes, nullptr, false, true);
}
TextureImage PreviewShoulders::readSkin(std::string path, std::string_view bytes) {
    if (bytes.size() > MaxModelResourceFile) throw std::runtime_error("Portrait skin exceeds model resource limit");
    auto skin = decodeTexture(std::move(path), {bytes.begin(), bytes.end()}, 16 * 1024 * 1024);
    std::vector<unsigned char>().swap(skin.encoded);
    return skin;
}
ModelPreview::ModelPreview(ModelPack const& source, std::shared_ptr<AnimationSampler const> bodySampler)
    : geometry(source.mainGeometry), textures(source.textures),
      background(source.previewBackground), foreground(source.previewForeground),
      noLighting(source.descriptor.at("properties").value("gui_no_lighting", false)),
      forceCutout(source.descriptor.at("properties").value("all_cutout", false)),
      sampler(bodySampler ? std::move(bodySampler) : std::make_shared<AnimationSampler const>(source.bodyAnimations(), geometry)),
      controllers(source.controllers,ControllerPolicy::SourcePackage) {
    evaluation.expressions.physics=std::make_shared<MolangPhysics>();
    auto fields=controllers.roamingFields(*sampler,false,RoamingLimit::LocalOnly);auto uiFields=ModelUiBindings(source.descriptor).roamingFields();fields.insert(uiFields.begin(),uiFields.end());
    evaluation.expressions.roaming.emplace(std::move(fields),RoamingRole::Producer,RoamingLimit::LocalOnly);
}
Json ModelPreview::report() const {return {{"expression_failures",evaluation.expressions.failures},{"diagnostics",evaluation.expressions.diagnostics},{"pose",AnimationSampler::report(lastPose)}};}
PreviewPoseView ModelPreview::samplePose(std::string const& clip, double time, std::size_t texture) const {
    auto& context = evaluation.expressions;
    context.queries["query.life_time"] = time;
    context.queries["query.anim_time"] = time;
    EntityQueryInput entity;
    entity.renderingInInventory = true;
    entity.health = entity.maxHealth = 20;
    entity.onGround = true;
    entity.tick = static_cast<std::int64_t>(time * 20);
    entity.renderTicks = time * 20;
    queries.bind(context, entity);
    if (!textures.empty()) context.queries["ysm.texture_name"] = textures[std::min(texture, textures.size() - 1)].name;
    HandInput input;
    input.enabled = true;
    // Java GUI entities stop the coded main predicate and loop the chosen clip in player.cap.
    lastPose = controllers.samplePlayer(*sampler, "", 0, time, evaluation, input, {}, clip.empty() ? "idle" : clip, time, true);
    context.soundCues.clear();
    context.particleCues.clear();
    return {lastPose, context.boneVisuals};
}
struct ModelPreview::GeometryFrame {
    std::vector<Face> faces;
    glm::dvec2 low{std::numeric_limits<double>::max()}, high{std::numeric_limits<double>::lowest()};
};
ModelPreview::GeometryFrame ModelPreview::collect(std::string const& clip, double time, PreviewCamera const& view,
    std::size_t texture, PreviewPose const* live, PreviewShoulders shoulders) const {
    GeometryFrame geometryFrame;
    auto& [faces, low, high] = geometryFrame;
    auto const& image=*textures[std::min(texture,textures.size()-1)].uv;
    auto const& model=geometry.at("minecraft:geometry").at(0);
    auto frame = live ? PreviewPoseView{live->bones, live->visuals} : samplePose(clip, time, texture);
    // The texture screen tilts the camera ten degrees and turns the model
    // about its own vertical axis. Pitch stays in screen space at every yaw.
    auto camera = rotate({10.0 - view.pitch, 0, 0}) * rotate({0, 180.0 + view.yaw, 0});
    std::array<std::optional<glm::dmat4>, 2> mounts{};
    auto append = [&](Json const& model, Pose const& pose, BoneVisuals const& visuals,
                      glm::dmat4 const& transformRoot, TextureImage const& skin, bool player) {
        auto const& bones = model.at("bones");
        std::map<std::string,Json const*> byName;std::map<std::string,glm::dmat4> transforms;std::set<std::string> visiting;
        for(auto const& b:bones)byName[b.at("name").get<std::string>()]=&b;
        std::function<glm::dmat4(std::string const&)> world=[&](std::string const& name)->glm::dmat4 {
            if(auto f=transforms.find(name);f!=transforms.end())return f->second;
            if(!byName.contains(name)||!visiting.insert(name).second)return glm::dmat4(1);
            auto const& b=*byName.at(name);auto pivot=sourcePoint(triple(field(b,"pivot")));auto r=sourceAngle(triple(field(b,"rotation")));auto pos=glm::dvec3(0),scale=glm::dvec3(1);
            if(auto p=pose.find(name);p!=pose.end()){pos=sourcePoint(vec(p->second.position));r+=sourceAngle(vec(p->second.rotation));scale=vec(p->second.scale);}
            auto m=glm::translate(glm::dmat4(1),pivot+pos)*rotate(r)*glm::scale(glm::dmat4(1),scale)*glm::translate(glm::dmat4(1),-pivot);
            if(b.contains("parent"))m=world(b["parent"].get_ref<std::string const&>())*m;
            visiting.erase(name);transforms[name]=m;return m;
        };
        auto const& description = field(model,"description");
        static const Json emptyDescription = Json::object();
        double tw = geometryTextureDimension(description.is_object()?description:emptyDescription, "texture_width");
        double th = geometryTextureDimension(description.is_object()?description:emptyDescription, "texture_height");
        constexpr int indices[6][4]={{6,7,4,5},{3,2,1,0},{7,3,0,4},{2,6,5,1},{7,6,2,3},{0,1,5,4}};
        constexpr char const* faceNames[]={"south","north","west","east","up","down"};
        constexpr int mirroredFaces[2][6]={{0,1,3,2,4,5},{0,1,3,2,5,4}};
        for(auto const& b:bones) {
            auto cubes = b.find("cubes");
            auto mesh = b.find("poly_mesh");
            if ((cubes == b.end() || cubes->empty()) && mesh == b.end()) continue;
            auto const& name = b.at("name").get_ref<std::string const&>();
            BoneVisual visual;
            if(auto v=visuals.find(name);v!=visuals.end())visual=v->second;
            if(visual.alpha==0)continue;
            auto transform = world(name);
            // A zero basis collapses every cube on this bone to a point. The
            // preview has no previous-pose interpolation, so skip it before UVs.
            auto zero = glm::dvec4(0.0);
            if (transform[0] == zero && transform[1] == zero && transform[2] == zero) continue;
            auto m = transformRoot * transform;
            if (mesh != b.end() && mesh->is_object() && mesh->contains("polys") && mesh->at("polys").is_array()
                && mesh->contains("positions") && mesh->contains("uvs")) {
                auto const& points = mesh->at("positions");
                auto const& textureCoordinates = mesh->at("uvs");
                auto multiplier = mesh->value("normalized_uvs", false) ? glm::dvec2(tw, th) : glm::dvec2(1);
                for (auto const& polygon : mesh->at("polys")) {
                    if (!polygon.is_array() || polygon.size() < 3 || polygon.size() > 4) continue;
                    Face face{};
                    bool valid = true;
                    for (std::size_t vertex = 0; vertex < 4; ++vertex) {
                        auto const& indices = polygon[std::min(vertex, polygon.size() - 1)];
                        if (!indices.is_array() || indices.size() != 3 || !indices[0].is_number_unsigned()
                            || !indices[2].is_number_unsigned()) { valid = false; break; }
                        auto pointIndex = indices[0].get<std::size_t>(), uvIndex = indices[2].get<std::size_t>();
                        if (pointIndex >= points.size() || uvIndex >= textureCoordinates.size()) { valid = false; break; }
                        auto const& uv = textureCoordinates[uvIndex];
                        if (!uv.is_array() || uv.size() != 2 || !uv[0].is_number() || !uv[1].is_number()) { valid = false; break; }
                        face.p[vertex] = glm::dvec3(m * glm::dvec4(sourcePoint(triple(points[pointIndex])), 1));
                        auto texel = glm::dvec2(uv[0].get<double>(), uv[1].get<double>()) * multiplier;
                        face.uv[vertex] = {texel.x, th - texel.y};
                    }
                    if (!valid) continue;
                    auto normal = glm::cross(face.p[1] - face.p[0], face.p[2] - face.p[0]);
                    if (glm::dot(normal,normal) < 1.e-16 || faces.size() >= 200000) continue;
                    for (auto const& point : face.p) {
                        low = glm::min(low, glm::dvec2(point.x, point.y));
                        high = glm::max(high, glm::dvec2(point.x, point.y));
                    }
                    face.light = .75 + .2 * std::abs(glm::normalize(normal).z);
                    face.visual = visual;
                    face.image = &skin; face.tw = tw; face.th = th; faces.push_back(face);
                }
            }
            if (cubes == b.end()) continue;
            auto ownerInflate = b.find("inflate");
            double boneInflate = ownerInflate != b.end() && ownerInflate->is_number() ? ownerInflate->get<double>() : 0.0;
            bool boneMirror = b.value("mirror", false);
            for(auto const& cube:*cubes) {
                bool oneSided = ((player && forceCutout) || signedClosedCube(cube, boneInflate)) && visual.alpha == 255 && visual.color == BoneVisual{}.color;
                auto origin=triple(field(cube,"origin")),dimensions=triple(field(cube,"size"));
                // The cube overrides its owning bone, including an explicit zero.
                auto inflateField = cube.find("inflate");
                double inflate = inflateField != cube.end() && inflateField->is_number()
                    ? inflateField->get<double>() : boneInflate;
                auto minimum = origin - glm::dvec3(inflate);
                auto maximum = origin + dimensions + glm::dvec3(inflate);
                auto cp=sourcePoint(triple(field(cube,"pivot")));auto cm=m*glm::translate(glm::dmat4(1),cp)*rotate(sourceAngle(triple(field(cube,"rotation"))))*glm::translate(glm::dmat4(1),-cp);
                glm::dvec3 v[8]={{minimum.x,minimum.y,minimum.z},{maximum.x,minimum.y,minimum.z},{maximum.x,maximum.y,minimum.z},{minimum.x,maximum.y,minimum.z},{minimum.x,minimum.y,maximum.z},{maximum.x,minimum.y,maximum.z},{maximum.x,maximum.y,maximum.z},{minimum.x,maximum.y,maximum.z}};
                for(auto& p:v)p=glm::dvec3(cm*glm::dvec4(sourcePoint(p),1));
                auto const& uv=field(cube,"uv");
                bool perFace=uv.is_object(), cubeMirror=cube.value("mirror", false);
                bool mirrorLayout=boneMirror || cubeMirror;
                auto offset=texturePair(uv);
                auto u=offset.x,vv=offset.y;
                auto a=std::floor(dimensions.x),z=std::floor(dimensions.z),h=std::floor(dimensions.y);
                double rects[6][4]={{u+z+a+z,vv+z,a,h},{u+z,vv+z,a,h},{u+z+a,vv+z,z,h},{u,vv+z,z,h},{u+z,vv,a,z},{u+z+a,vv+z,a,-z}};
                for(int f=0;f<6;++f) {
                    auto layout = mirrorLayout ? mirroredFaces[perFace][f] : f;
                    double u0=rects[layout][0],v0=rects[layout][1],uw=rects[layout][2],vh=rects[layout][3];
                    if(perFace) {
                        auto authored=uv.find(faceNames[layout]);
                        if(authored==uv.end()||!authored->is_object())continue;
                        auto pos=texturePair(field(*authored,"uv"));
                        // The 2.6.5 preview samples the UV origin if the size is omitted.
                        auto sz=texturePair(field(*authored,"uv_size"));
                        u0=pos.x;v0=pos.y;uw=sz.x;vh=sz.y;
                    }
                    Face face;for(int i=0;i<4;++i)face.p[i]=v[indices[f][i]];
                    // Hidden scale-zero bones have no drawable surface. Their
                    // distant pivots must not shrink the visible character.
                    auto cross=glm::cross(face.p[1]-face.p[0],face.p[3]-face.p[0]);
                    if(glm::dot(cross,cross)<1.e-16 || (oneSided && cross.z >= 0))continue;
                    for(auto const& point:face.p){low=glm::min(low,glm::dvec2(point.x,point.y));high=glm::max(high,glm::dvec2(point.x,point.y));}
                    auto u1=u0+uw;
                    if(cubeMirror)std::swap(u0,u1);
                    face.uv[0]={u0,v0};face.uv[1]={u1,v0};face.uv[2]={u1,v0+vh};face.uv[3]={u0,v0+vh};
                    face.light=f==4?1:f==5?.6:f==1?.94:.8;face.visual=visual;face.image = &skin; face.tw = tw; face.th = th; faces.push_back(face);
                }
            }
        }
        if (player && live) {
            for (std::size_t side = 0; side < mounts.size(); ++side) {
                auto name = side == 0 ? "LeftShoulderLocator" : "RightShoulderLocator";
                auto it = byName.find(name);
                if (it == byName.end()) continue;
                auto pivot = sourcePoint(triple(field(*it->second,"pivot")));
                mounts[side] = glm::translate(transformRoot * world(name), pivot);
            }
        }
    };
    append(model, frame.bones, frame.visuals, camera, image, true);
    auto playerLow = low, playerHigh = high;
    if (shoulders.model && shoulders.pose) {
        for (std::size_t side = 0; side < mounts.size(); ++side) {
            if (!mounts[side] || !shoulders.skins[side]) continue;
            auto start = faces.size();
            auto bodyLow = low, bodyHigh = high;
            try {
                append(*shoulders.model, *shoulders.pose, {}, *mounts[side], *shoulders.skins[side], false);
            } catch (Json::exception const&) {
                // A malformed active resource pack must leave the player and any other shoulder intact.
                faces.resize(start);
                low = bodyLow;
                high = bodyHigh;
            }
        }
    }
    low = playerLow;
    high = playerHigh;
    return geometryFrame;
}

PreviewMesh ModelPreview::mesh(std::string const& clip, double time, PreviewCamera const& view, std::size_t texture, PreviewPose const* live) const {
    PreviewMesh output;
    if (textures.empty()) return output;
    output.skin = textures[std::min(texture, textures.size() - 1)].uv;
    if (!live) { output.background = background; output.foreground = foreground; }
    auto [faces, low, high] = collect(clip, time, view, texture, live, {});
    if (faces.empty() || !std::isfinite(high.x - low.x) || !std::isfinite(high.y - low.y)) return output;
    auto center = (high + low) * .5;
    double scale = (1.0 - 16.0 / BodyFramePixels) * view.zoom / std::max({high.x-low.x, high.y-low.y, 1.0});
    // Translucent fragments are blended back to front after the opaque depth pass.
    double nearZ = std::numeric_limits<double>::lowest(), farZ = std::numeric_limits<double>::max();
    std::vector<std::pair<double, std::size_t>> order;
    order.reserve(faces.size());
    for (std::size_t i = 0; i < faces.size(); ++i) {
        auto const& p = faces[i].p;
        auto depth = p[0].z + p[1].z + p[2].z + p[3].z;
        if (!std::isfinite(depth)) continue;
        order.emplace_back(depth, i);
        for (auto const& point : p) {
            nearZ = std::max(nearZ, point.z);
            farZ = std::min(farZ, point.z);
        }
    }
    std::sort(order.begin(), order.end());
    output.vertices.reserve(order.size() * 6);
    for (auto const& entry : order) {
        auto const& face = faces[entry.second];
        auto channel = [&](int n) { return static_cast<std::uint32_t>(face.visual.color[n] * (noLighting ? 1.0 : face.light)); };
        auto color = (std::uint32_t(face.visual.alpha) << 24) | (channel(0) << 16) | (channel(1) << 8) | channel(2);
        PreviewMeshVertex quad[4];
        for (int i = 0; i < 4; ++i) {
            auto const& p = face.p[i];
            quad[i] = {static_cast<float>(.5 + view.panX + (p.x - center.x) * scale),
                       static_cast<float>(.5 + view.panY - (p.y - center.y) * scale),
                       nearZ == farZ ? .5f : static_cast<float>((nearZ - p.z) / (nearZ - farZ)),
                       static_cast<float>(face.uv[i].x / face.tw), static_cast<float>(face.uv[i].y / face.th), color};
        }
        for (int i : {1, 2}) {
            auto a = quad[0], b = quad[i], c = quad[i+1];
            // Front-facing UI triangles use the same winding as the menu rectangles.
            auto cross = (b.x-a.x)*(c.y-a.y) - (c.x-a.x)*(b.y-a.y);
            if (!std::isfinite(cross) || cross == 0) continue;
            if (cross > 0) std::swap(b,c);
            output.vertices.insert(output.vertices.end(), {a,b,c});
        }
    }
    return output;
}
TextureImage ModelPreview::render(std::string clip,double time,PreviewCamera const& view,std::size_t texture,
    bool decorations, PreviewPose const* live, PreviewShoulders shoulders) const {
    // Leave the player's existing HUD framing intact when an attachment extends above it.
    bool perched = live && (shoulders.skins[0] || shoulders.skins[1]);
    int size = BodyFramePixels + (perched ? BodyFramePixels / 2 : 0);
    TextureImage result;result.width=size;result.height=size;result.rgba.resize(size*size*4);result.format="rgba";
    if(textures.empty())return result;
    auto [faces, low, high] = collect(clip, time, view, texture, live, shoulders);
    auto finish = [&]() -> TextureImage {
        if (decorations && background) {
            auto model = std::move(result.rgba);
            result.rgba.resize(size * size * 4);
            TextureImage layer;
            layer.width = size; layer.height = size; layer.rgba = std::move(model);
            composite(result, *background);
            composite(result, layer);
        }
        if (decorations && foreground) composite(result, *foreground);
        return std::move(result);
    };
    if(faces.empty()||!std::isfinite(high.x-low.x)||!std::isfinite(high.y-low.y))return finish();
    double scale=(BodyFramePixels-16)/std::max({high.x-low.x,high.y-low.y,1.0})*view.zoom;auto center=(high+low)*.5;
    std::vector<double> depth(size*size,std::numeric_limits<double>::lowest());
    for(auto const& face:faces)for(int tri=0;tri<2;++tri) {
        auto const& image = *face.image;
        double tw = face.tw, th = face.th;
        int ids[3]={0,tri?2:1,tri?3:2};glm::dvec3 p[3];glm::dvec2 uv[3];
        for(int i=0;i<3;++i){auto source=face.p[ids[i]];p[i]={(source.x-center.x)*scale+size*.5+BodyFramePixels*view.panX,
            size*.5+BodyFramePixels*view.panY-(source.y-center.y)*scale,source.z};uv[i]=face.uv[ids[i]];}
        double area=(p[1].x-p[0].x)*(p[2].y-p[0].y)-(p[1].y-p[0].y)*(p[2].x-p[0].x);if(std::abs(area)<.000001)continue;
        int minx=std::max(0,static_cast<int>(std::floor(std::min({p[0].x,p[1].x,p[2].x})))),maxx=std::min(size-1,static_cast<int>(std::ceil(std::max({p[0].x,p[1].x,p[2].x}))));
        int miny=std::max(0,static_cast<int>(std::floor(std::min({p[0].y,p[1].y,p[2].y})))),maxy=std::min(size-1,static_cast<int>(std::ceil(std::max({p[0].y,p[1].y,p[2].y}))));
        for(int y=miny;y<=maxy;++y)for(int x=minx;x<=maxx;++x) {
            double b=((x+.5-p[0].x)*(p[2].y-p[0].y)-(y+.5-p[0].y)*(p[2].x-p[0].x))/area;
            double c=((p[1].x-p[0].x)*(y+.5-p[0].y)-(p[1].y-p[0].y)*(x+.5-p[0].x))/area;double a=1-b-c;
            if(a<0||b<0||c<0)continue;double z=a*p[0].z+b*p[1].z+c*p[2].z;auto pixel=y*size+x;if(z<depth[pixel])continue;
            auto t=uv[0]+b*(uv[1]-uv[0])+c*(uv[2]-uv[0]);int tx=std::clamp(static_cast<int>(t.x/tw*image.width),0,static_cast<int>(image.width)-1),ty=std::clamp(static_cast<int>(t.y/th*image.height),0,static_cast<int>(image.height)-1);
            auto offset=(ty*image.width+tx)*4;if(image.rgba[offset+3]<32)continue;
            depth[pixel]=z;for(int k=0;k<3;++k)result.rgba[pixel*4+k]=static_cast<unsigned char>(image.rgba[offset+k]*(noLighting ? 1.0 : face.light)*face.visual.color[k]/255);result.rgba[pixel*4+3]=static_cast<unsigned char>(image.rgba[offset+3]*face.visual.alpha/255);
        }
    }
    return finish();
}
}
