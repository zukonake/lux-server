#include <alias/ref.hpp>
#include <world/entity/common.hpp>
#include "world.hpp"

namespace world
{

World::World(data::Config const &config) :
    config(config),
    map(config)
{

}

Tile &World::operator[](MapPoint const &pos)
{
    return map[pos];
}

Tile const &World::operator[](MapPoint const &pos) const
{
    return map[pos];
}

Entity &World::create_player()
{
    util::log("WORLD", util::DEBUG, "created player");
    return create_entity(*config.player_type, EntityPoint(0, 0, 0));
}

}
