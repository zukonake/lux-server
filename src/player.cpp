#include <alias/int.hpp>
#include <world/tile/type.hpp>
#include <world/map/common.hpp>
#include <world/entity.hpp>
#include <world.hpp>
#include <net/server_data.hpp>
#include <net/client_data.hpp>
#include "player.hpp"

Player::Player(ENetPeer *peer, world::Entity &entity) :
    peer(peer),
    entity(&entity)
{

}

void Player::receive(ENetPacket *packet)
{
    Vector<U8> bytes(packet->data, packet->data + packet->dataLength);
    net::ClientData client_data;
    net::deserialize(bytes, client_data);
    view_size = client_data.view_size;
}

ENetPacket *Player::send() const
{
    //TODO store buffer in Player?
    Vector<net::TileState> tiles;
    tiles.reserve(view_size.x * view_size.y);
    world::MapPoint offset;
    world::MapPoint entity_pos = (world::MapPoint)entity->get_pos();
    offset.z = 0;
    for(offset.y = -(int)((view_size.y + 1) / 2); offset.y < view_size.y / 2; ++offset.y)
    {
        for(offset.x = -(int)((view_size.x + 1) / 2); offset.x < view_size.x / 2; ++offset.x)
        {
            world::MapPoint tile_pos = entity_pos + offset;
            auto const &tile_type = entity->world[tile_pos].type;
            tiles.emplace_back();
            tiles.back().shape = (net::TileState::Shape)tile_type->shape;
            tiles.back().tex_pos = tile_type->tex_pos;
        }
    }
    net::ServerData server_data = {tiles};
    Vector<U8> bytes;
    net::serialize(bytes, server_data);
    return enet_packet_create(bytes.data(), bytes.size(), 0);
}
