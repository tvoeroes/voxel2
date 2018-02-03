#pragma once

#include <unordered_map>
#include <glm/vec3.hpp>
#include "gl3w.h"
#include "VoxelStorage.hpp"
#include "MeshIterator.hpp"

class Scene {
public:
    Scene() : m_mesh_iterator{ 1 } {

    }

    void update(VoxelStorage & vs, const glm::ivec3 & center) {
        for (size_t i = 0, size = m_mesh_iterator.size(); i < size; ++i) {
            const auto mesh_position = m_mesh_iterator.get(i) + center;
            const auto mesh_iterator = m_meshes.find(mesh_position);
            if (mesh_iterator == m_meshes.end()) {
                const auto chunk_mesh = generateAndUploadChunkMesh(vs, mesh_position);
                m_meshes.insert({ mesh_position, chunk_mesh });

                break; // TODO: improve
            }
            // TODO: remove out of range
        }
    }

    void draw(GLint offset_uniform) {
        auto s = m_meshes.size();
        const auto CHUNK_SIZES = glm::ivec3{ VoxelStorage::CHUNK_SIZE.x, VoxelStorage::CHUNK_SIZE.y, VoxelStorage::CHUNK_SIZE.z };
        for (const auto & m : m_meshes) {
            if (m.second.element_count > 0) {
                const auto offset = m.first * CHUNK_SIZES;
                glUniform3f(offset_uniform, offset.x, offset.y, offset.z);
                glBindVertexArray(m.second.VAO);
                glDrawArrays(GL_TRIANGLES, 0, m.second.element_count);
                glBindVertexArray(0);
            }
        }
    }

private:
    //struct VoxelMeshVertex {
    //    uint8_t x, y, z;
    //};

    struct ChunkMesh {
        GLuint VAO, VBO;
        GLsizei element_count;
    };
    struct KeyHash {
    std::size_t operator () (const glm::ivec3 & k) const {
        return k.x * k.y * k.z; // TODO: better hash function
    }};
    struct KeyEqual {
    bool operator()(const glm::ivec3 & lhs, const glm::ivec3 & rhs) const {
        return lhs.x == rhs.x && lhs.y == rhs.y && lhs.z == rhs.z;
    }};
    std::unordered_map<glm::ivec3, ChunkMesh, KeyHash, KeyEqual> m_meshes;
    MeshIterator m_mesh_iterator;

    std::vector<uint8_t> generateChunkMesh(VoxelStorage & vs, const glm::ivec3 & chunk_position);

    ChunkMesh generateAndUploadChunkMesh(VoxelStorage & vs, const glm::ivec3 & chunk_position) {
        std::vector<uint8_t> mesh = generateChunkMesh(vs, chunk_position);

        /*uint8_t dummy_mesh[] = {
            5, 5, 5,
            5, 6, 5,
            5, 6, 6
        };
        for (const auto & i : dummy_mesh) mesh.push_back(i);*/

        ChunkMesh chunk_mesh;
        chunk_mesh.element_count = 10; // TODO: correct value

        glGenVertexArrays(1, &chunk_mesh.VAO);
        glGenBuffers(1, &chunk_mesh.VBO);
        glBindVertexArray(chunk_mesh.VAO);
        glBindBuffer(GL_ARRAY_BUFFER, chunk_mesh.VBO);
        //QuadEBO::bind();
        // TODO: quadebo resize
        glVertexAttribIPointer(0, 3, GL_UNSIGNED_BYTE, sizeof(uint8_t) * 3, (GLvoid *)(0));
        glEnableVertexAttribArray(0);
        glBindVertexArray(0);

        glBindBuffer(GL_ARRAY_BUFFER, chunk_mesh.VBO);
        // TODO
        glBufferData(GL_ARRAY_BUFFER, mesh.size() * sizeof(mesh[0]), mesh.data(), GL_STATIC_DRAW);

        return chunk_mesh;
    }
};
