#include "GltfImporter.h"

#include "Foundation/Log.h"

#define CGLTF_IMPLEMENTATION
#include "cgltf.h"

#include <DirectXMath.h>
#include <algorithm>
#include <cstdint>
#include <cstring>

namespace
{
    using Foundation::Log::Severity;

    // cgltf keeps every array (nodes, meshes, materials, skins, textures) flat off
    // cgltf_data, and every cross-reference (a node's mesh, a skin's joint, ...) as a raw
    // pointer into one of those arrays - so "which index is this" is always pointer
    // arithmetic against the owning array's base pointer. Centralized here since every
    // Build*() function below needs it at least once.
    int32_t IndexOf(const void* element, const void* arrayBase, size_t elementSize)
    {
        if (element == nullptr)
        {
            return Asset::kInvalidIndex;
        }
        const ptrdiff_t byteOffset = static_cast<const uint8_t*>(element) - static_cast<const uint8_t*>(arrayBase);
        return static_cast<int32_t>(byteOffset / static_cast<ptrdiff_t>(elementSize));
    }

    int32_t NodeIndex(const cgltf_data& data, const cgltf_node* node)
    {
        return IndexOf(node, data.nodes, sizeof(cgltf_node));
    }

    int32_t MeshIndex(const cgltf_data& data, const cgltf_mesh* mesh)
    {
        return IndexOf(mesh, data.meshes, sizeof(cgltf_mesh));
    }

    int32_t SkinIndex(const cgltf_data& data, const cgltf_skin* skin)
    {
        return IndexOf(skin, data.skins, sizeof(cgltf_skin));
    }

    int32_t MaterialIndex(const cgltf_data& data, const cgltf_material* material)
    {
        return IndexOf(material, data.materials, sizeof(cgltf_material));
    }

    int32_t TextureIndex(const cgltf_data& data, const cgltf_texture_view& textureView)
    {
        return IndexOf(textureView.texture, data.textures, sizeof(cgltf_texture));
    }

    const cgltf_attribute* FindAttribute(const cgltf_primitive& primitive, cgltf_attribute_type type, cgltf_int index = 0)
    {
        for (cgltf_size i = 0; i < primitive.attributes_count; ++i)
        {
            const cgltf_attribute& attribute = primitive.attributes[i];
            if (attribute.type == type && attribute.index == index)
            {
                return &attribute;
            }
        }
        return nullptr;
    }

    void BuildTextures(const cgltf_data& data, Asset::Model& outModel)
    {
        outModel.textures.resize(data.textures_count);
        for (cgltf_size i = 0; i < data.textures_count; ++i)
        {
            const cgltf_texture& texture = data.textures[i];
            if (texture.image != nullptr && texture.image->uri != nullptr)
            {
                outModel.textures[i].filePath = texture.image->uri;
            }
            else
            {
                // Embedded (data-URI or .glb buffer-view) images have no on-disk file path.
                // TODO(OM): support embedded image data once a source asset actually uses it -
                // every source asset tested so far uses loose texture files, not embedded
                // ones, so this hasn't come up yet.
                Foundation::Log::Write(Severity::Warning, "Asset", "Texture %zu has no file URI (embedded image?); leaving its path empty", i);
            }
        }
    }

    void BuildMaterials(const cgltf_data& data, Asset::Model& outModel)
    {
        outModel.materials.resize(data.materials_count);
        for (cgltf_size i = 0; i < data.materials_count; ++i)
        {
            const cgltf_material& material = data.materials[i];
            Asset::Material& outMaterial = outModel.materials[i];

            outMaterial.name = material.name != nullptr ? material.name : "";

            if (material.has_pbr_metallic_roughness)
            {
                const cgltf_pbr_metallic_roughness& pbr = material.pbr_metallic_roughness;
                std::memcpy(outMaterial.baseColorFactor, pbr.base_color_factor, sizeof(outMaterial.baseColorFactor));
                outMaterial.metallicFactor = pbr.metallic_factor;
                outMaterial.roughnessFactor = pbr.roughness_factor;
                outMaterial.baseColorTexture = TextureIndex(data, pbr.base_color_texture);
                outMaterial.metallicRoughnessTexture = TextureIndex(data, pbr.metallic_roughness_texture);
            }

            std::memcpy(outMaterial.emissiveFactor, material.emissive_factor, sizeof(outMaterial.emissiveFactor));
            outMaterial.normalTexture = TextureIndex(data, material.normal_texture);
            outMaterial.emissiveTexture = TextureIndex(data, material.emissive_texture);

            switch (material.alpha_mode)
            {
                case cgltf_alpha_mode_mask: outMaterial.alphaMode = Asset::AlphaMode::Mask; break;
                case cgltf_alpha_mode_blend: outMaterial.alphaMode = Asset::AlphaMode::Blend; break;
                default: outMaterial.alphaMode = Asset::AlphaMode::Opaque; break;
            }
            outMaterial.alphaCutoff = material.alpha_cutoff;
            outMaterial.doubleSided = material.double_sided != 0;
        }
    }

    void BuildPrimitive(const cgltf_data& data, const cgltf_primitive& primitive, const char* meshName, Asset::Primitive& outPrimitive)
    {
        outPrimitive.materialIndex = MaterialIndex(data, primitive.material);

        const cgltf_attribute* positionAttr = FindAttribute(primitive, cgltf_attribute_type_position);
        if (positionAttr == nullptr)
        {
            Foundation::Log::Write(Severity::Warning, "Asset", "Mesh '%s' has a primitive with no POSITION attribute; skipping it", meshName);
            return;
        }

        const cgltf_attribute* normalAttr = FindAttribute(primitive, cgltf_attribute_type_normal);
        const cgltf_attribute* uv0Attr = FindAttribute(primitive, cgltf_attribute_type_texcoord, 0);
        const cgltf_attribute* tangentAttr = FindAttribute(primitive, cgltf_attribute_type_tangent);
        const cgltf_attribute* jointsAttr = FindAttribute(primitive, cgltf_attribute_type_joints, 0);
        const cgltf_attribute* weightsAttr = FindAttribute(primitive, cgltf_attribute_type_weights, 0);

        const cgltf_size vertexCount = positionAttr->data->count;
        outPrimitive.vertices.resize(vertexCount);
        if (jointsAttr != nullptr && weightsAttr != nullptr)
        {
            outPrimitive.skinning.resize(vertexCount);
        }

        for (cgltf_size v = 0; v < vertexCount; ++v)
        {
            Asset::Vertex& vertex = outPrimitive.vertices[v];
            cgltf_accessor_read_float(positionAttr->data, v, &vertex.position.x, 3);
            if (normalAttr != nullptr)
            {
                cgltf_accessor_read_float(normalAttr->data, v, &vertex.normal.x, 3);
            }
            if (uv0Attr != nullptr)
            {
                cgltf_accessor_read_float(uv0Attr->data, v, &vertex.uv0.x, 2);
            }
            if (tangentAttr != nullptr)
            {
                cgltf_accessor_read_float(tangentAttr->data, v, &vertex.tangent.x, 4);
            }

            if (!outPrimitive.skinning.empty())
            {
                // Engine-importer gotcha: JOINTS_0 values index into this *primitive's skin's*
                // local joints array (Model::skins[skinIndex].joints), never Model::nodes
                // directly - stored through unchanged here so Skin.h's Joint order is what
                // gives them meaning later.
                unsigned int joints[4] = {};
                float weights[4] = {};
                cgltf_accessor_read_uint(jointsAttr->data, v, joints, 4);
                cgltf_accessor_read_float(weightsAttr->data, v, weights, 4);

                Asset::SkinningData& skinning = outPrimitive.skinning[v];
                for (int c = 0; c < 4; ++c)
                {
                    skinning.jointIndices[c] = static_cast<uint16_t>(joints[c]);
                    skinning.jointWeights[c] = weights[c];
                }
            }
        }

        if (primitive.indices != nullptr)
        {
            outPrimitive.indices.resize(primitive.indices->count);
            cgltf_accessor_unpack_indices(primitive.indices, outPrimitive.indices.data(), sizeof(uint32_t), primitive.indices->count);
        }
        // else: non-indexed primitive - outPrimitive.indices stays empty, meaning "draw
        // vertices directly" to whatever consumes this later (no synthetic 0..N-1 index
        // list manufactured here).
    }

    void BuildMeshes(const cgltf_data& data, Asset::Model& outModel)
    {
        outModel.meshes.resize(data.meshes_count);
        for (cgltf_size i = 0; i < data.meshes_count; ++i)
        {
            const cgltf_mesh& mesh = data.meshes[i];
            Asset::Mesh& outMesh = outModel.meshes[i];
            outMesh.name = mesh.name != nullptr ? mesh.name : "";

            for (cgltf_size p = 0; p < mesh.primitives_count; ++p)
            {
                const cgltf_primitive& primitive = mesh.primitives[p];
                if (primitive.type != cgltf_primitive_type_triangles)
                {
                    Foundation::Log::Write(Severity::Warning, "Asset", "Mesh '%s' has a non-triangle primitive; skipping it", outMesh.name.c_str());
                    continue;
                }

                Asset::Primitive outPrimitive;
                BuildPrimitive(data, primitive, outMesh.name.c_str(), outPrimitive);
                if (!outPrimitive.vertices.empty())
                {
                    outMesh.primitives.push_back(std::move(outPrimitive));
                }
            }
        }
    }

    void BuildSkins(const cgltf_data& data, Asset::Model& outModel)
    {
        outModel.skins.resize(data.skins_count);
        for (cgltf_size i = 0; i < data.skins_count; ++i)
        {
            const cgltf_skin& skin = data.skins[i];
            Asset::Skin& outSkin = outModel.skins[i];
            outSkin.name = skin.name != nullptr ? skin.name : "";
            outSkin.joints.resize(skin.joints_count);

            for (cgltf_size j = 0; j < skin.joints_count; ++j)
            {
                const cgltf_node* jointNode = skin.joints[j];
                Asset::Joint& outJoint = outSkin.joints[j];
                outJoint.name = jointNode->name != nullptr ? jointNode->name : "";
                outJoint.nodeIndex = NodeIndex(data, jointNode);

                // A joint's parent joint isn't necessarily its node's immediate parent - walk
                // up until another member of this same skin's joint set is found (or the
                // hierarchy runs out, meaning this is a root joint).
                for (const cgltf_node* ancestor = jointNode->parent; ancestor != nullptr; ancestor = ancestor->parent)
                {
                    bool found = false;
                    for (cgltf_size k = 0; k < skin.joints_count; ++k)
                    {
                        if (skin.joints[k] == ancestor)
                        {
                            outJoint.parentJointIndex = static_cast<int32_t>(k);
                            found = true;
                            break;
                        }
                    }
                    if (found)
                    {
                        break;
                    }
                }

                if (skin.inverse_bind_matrices != nullptr)
                {
                    float raw[16];
                    cgltf_accessor_read_float(skin.inverse_bind_matrices, j, raw, 16);
                    // glTF matrices are column-major; a direct 16-float copy into this
                    // row-major-storage struct is the standard transpose that makes the same
                    // rigid transform correct under DirectXMath's row-vector (v' = v * M)
                    // convention - not an uninterpreted/raw copy.
                    std::memcpy(&outJoint.inverseBindMatrix, raw, sizeof(raw));
                }
            }
        }
    }

    Asset::AnimationPath ToAnimationPath(cgltf_animation_path_type path, bool& outSupported)
    {
        outSupported = true;
        switch (path)
        {
            case cgltf_animation_path_type_translation: return Asset::AnimationPath::Translation;
            case cgltf_animation_path_type_rotation: return Asset::AnimationPath::Rotation;
            case cgltf_animation_path_type_scale: return Asset::AnimationPath::Scale;
            default:
                outSupported = false;
                return Asset::AnimationPath::Translation;
        }
    }

    void BuildClips(const cgltf_data& data, Asset::Model& outModel)
    {
        outModel.clips.resize(data.animations_count);
        for (cgltf_size i = 0; i < data.animations_count; ++i)
        {
            const cgltf_animation& animation = data.animations[i];
            Asset::Clip& outClip = outModel.clips[i];
            outClip.name = animation.name != nullptr ? animation.name : "";

            for (cgltf_size c = 0; c < animation.channels_count; ++c)
            {
                const cgltf_animation_channel& channel = animation.channels[c];
                const cgltf_animation_sampler& sampler = *channel.sampler;

                if (sampler.interpolation == cgltf_interpolation_type_cubic_spline)
                {
                    // TODO(OM): handle cubic-spline interpolation (in/out tangents alongside
                    // each value) once a source clip actually uses it - the validated
                    // stick-figure export bakes to plain per-frame samples, so this hasn't
                    // come up yet.
                    Foundation::Log::Write(Severity::Warning, "Asset", "Clip '%s' has a cubic-spline channel; skipping it", outClip.name.c_str());
                    continue;
                }

                bool pathSupported = false;
                Asset::AnimationPath path = ToAnimationPath(channel.target_path, pathSupported);
                if (!pathSupported)
                {
                    // Most notably morph-target "weights" channels - see AnimationPath::Weights TODO(OM) in Clip.h.
                    Foundation::Log::Write(Severity::Warning, "Asset", "Clip '%s' has an unsupported animation path; skipping it", outClip.name.c_str());
                    continue;
                }

                Asset::AnimationChannel outChannel;
                outChannel.targetNodeIndex = NodeIndex(data, channel.target_node);
                outChannel.path = path;

                const cgltf_size keyCount = sampler.input->count;
                outChannel.keyTimes.resize(keyCount);
                outChannel.keyValues.resize(keyCount);

                const cgltf_size componentCount = (path == Asset::AnimationPath::Rotation) ? 4 : 3;
                for (cgltf_size k = 0; k < keyCount; ++k)
                {
                    cgltf_accessor_read_float(sampler.input, k, &outChannel.keyTimes[k], 1);

                    float value[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
                    cgltf_accessor_read_float(sampler.output, k, value, componentCount);
                    outChannel.keyValues[k] = DirectX::XMFLOAT4(value[0], value[1], value[2], value[3]);
                }

                if (keyCount > 0)
                {
                    outClip.durationSeconds = std::max(outClip.durationSeconds, outChannel.keyTimes.back());
                }

                outClip.channels.push_back(std::move(outChannel));
            }
        }
    }

    void BuildNodes(const cgltf_data& data, Asset::Model& outModel)
    {
        outModel.nodes.resize(data.nodes_count);
        for (cgltf_size i = 0; i < data.nodes_count; ++i)
        {
            const cgltf_node& node = data.nodes[i];
            Asset::Node& outNode = outModel.nodes[i];
            outNode.name = node.name != nullptr ? node.name : "";
            outNode.meshIndex = MeshIndex(data, node.mesh);
            outNode.skinIndex = SkinIndex(data, node.skin);

            if (node.has_matrix)
            {
                DirectX::XMFLOAT4X4 matrix;
                // Same column-major -> row-major-storage transpose trick as the inverse bind
                // matrices above.
                std::memcpy(&matrix, node.matrix, sizeof(matrix));

                DirectX::XMVECTOR scale, rotation, translation;
                if (DirectX::XMMatrixDecompose(&scale, &rotation, &translation, DirectX::XMLoadFloat4x4(&matrix)))
                {
                    DirectX::XMStoreFloat3(&outNode.scale, scale);
                    DirectX::XMStoreFloat4(&outNode.rotation, rotation);
                    DirectX::XMStoreFloat3(&outNode.translation, translation);
                }
                else
                {
                    Foundation::Log::Write(Severity::Warning, "Asset", "Node '%s' has a non-decomposable matrix transform; using identity", outNode.name.c_str());
                }
            }
            else
            {
                if (node.has_translation)
                {
                    outNode.translation = DirectX::XMFLOAT3(node.translation);
                }
                if (node.has_rotation)
                {
                    // glTF quaternion component order (x, y, z, w) matches XMFLOAT4 as used
                    // here directly - no reordering needed.
                    outNode.rotation = DirectX::XMFLOAT4(node.rotation);
                }
                if (node.has_scale)
                {
                    outNode.scale = DirectX::XMFLOAT3(node.scale);
                }
            }

            outNode.childNodeIndices.reserve(node.children_count);
            for (cgltf_size c = 0; c < node.children_count; ++c)
            {
                outNode.childNodeIndices.push_back(NodeIndex(data, node.children[c]));
            }
        }

        const cgltf_scene* scene = data.scene != nullptr ? data.scene : (data.scenes_count > 0 ? &data.scenes[0] : nullptr);
        if (scene != nullptr)
        {
            for (cgltf_size i = 0; i < scene->nodes_count; ++i)
            {
                outModel.rootNodeIndices.push_back(NodeIndex(data, scene->nodes[i]));
            }
        }
        else
        {
            // No scene at all (unusual, but not invalid glTF): fall back to every node with no
            // parent, which is the only other sensible definition of "a root" available here.
            for (cgltf_size i = 0; i < data.nodes_count; ++i)
            {
                if (data.nodes[i].parent == nullptr)
                {
                    outModel.rootNodeIndices.push_back(static_cast<int32_t>(i));
                }
            }
        }
    }

    void LogModelSummary(const char* filePath, const Asset::Model& model)
    {
        size_t primitiveCount = 0;
        for (const Asset::Mesh& mesh : model.meshes)
        {
            primitiveCount += mesh.primitives.size();
        }

        Foundation::Log::Write(
            Severity::Info,
            "Asset",
            "Imported '%s': %zu mesh(es)/%zu primitive(s), %zu material(s), %zu texture(s), %zu node(s), %zu skin(s), %zu clip(s)",
            filePath,
            model.meshes.size(),
            primitiveCount,
            model.materials.size(),
            model.textures.size(),
            model.nodes.size(),
            model.skins.size(),
            model.clips.size());

        for (const Asset::Skin& skin : model.skins)
        {
            Foundation::Log::Write(Severity::Info, "Asset", "  Skin '%s': %zu joint(s)", skin.name.c_str(), skin.joints.size());
        }
        for (const Asset::Clip& clip : model.clips)
        {
            Foundation::Log::Write(Severity::Info, "Asset", "  Clip '%s': %zu channel(s), %.3fs", clip.name.c_str(), clip.channels.size(), clip.durationSeconds);
        }
    }
}

namespace Asset
{
    bool ImportGltf(const char* filePath, Model& outModel)
    {
        outModel = Model{};

        cgltf_options options = {};
        cgltf_data* data = nullptr;
        cgltf_result result = cgltf_parse_file(&options, filePath, &data);
        if (result != cgltf_result_success)
        {
            Foundation::Log::Write(Severity::Error, "Asset", "Failed to parse glTF '%s' (cgltf_result %d)", filePath, static_cast<int>(result));
            return false;
        }

        result = cgltf_load_buffers(&options, data, filePath);
        if (result != cgltf_result_success)
        {
            Foundation::Log::Write(Severity::Error, "Asset", "Failed to load buffers for glTF '%s' (cgltf_result %d)", filePath, static_cast<int>(result));
            cgltf_free(data);
            return false;
        }

        if (cgltf_validate(data) != cgltf_result_success)
        {
            // Non-fatal: some real-world exporters produce files that fail cgltf's strict
            // validation but still parse and contain everything this importer needs.
            Foundation::Log::Write(Severity::Warning, "Asset", "glTF '%s' failed cgltf_validate; continuing anyway", filePath);
        }

        BuildTextures(*data, outModel);
        BuildMaterials(*data, outModel);
        BuildMeshes(*data, outModel);
        BuildSkins(*data, outModel);
        BuildClips(*data, outModel);
        BuildNodes(*data, outModel);

        cgltf_free(data);

        LogModelSummary(filePath, outModel);
        return true;
    }
}
