#pragma once

#include <world/map/chunk/common.hpp>

namespace data { struct Config; }

namespace world
{

struct Chunk;

class Generator
{
    public:
    Generator(data::Config const &config);
    Generator &operator=(Generator const &that) = delete;

    void generate_chunk(Chunk &chunk, ChunkPoint pos);
    private:
    data::Config const &config;
};

}
