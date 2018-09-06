#include <algorithm>
//
#include <glm/glm.hpp>
//
#include <lux/alias/scalar.hpp>
#include <lux/alias/string.hpp>
#include <lux/util/log.hpp>
#include <lux/common.hpp>
#include <lux/world/entity.hpp>
#include <lux/world/map.hpp>
#include <lux/net/server/packet.hpp>
#include <lux/net/client/packet.hpp>
//
#include <map/voxel_type.hpp>
#include <entity/entity.hpp>
#include <world.hpp>
#include "player.hpp"

Player::Player(data::Config const &conf, ENetPeer *peer, Entity &entity) :
    peer(peer),
    conf(conf),
    entity(&entity),
    load_range(1.f),
    sent_init(false),
    received_init(false)
{

}

void Player::receive(net::client::Packet const &cp)
{
    if(!received_init)
    {
        if(cp.type == net::client::Packet::INIT)
        {
            init_from_client(cp.init);
            received_init = true;
        }
        else
        {
            lux::error("PLAYER", "client has not sent init data");
            //TODO just kick him out
        }
    }
    else if(cp.type == net::client::Packet::TICK)
    {
        auto h_dir = 0.2f * cp.tick.character_dir;
        if(cp.tick.is_moving)  entity->move({h_dir.x, h_dir.y, 0.0});
        if(cp.tick.is_jumping) entity->jump();
    }
    else if(cp.type == net::client::Packet::CONF)
    {
        change_config(cp.conf);
    }
}

void Player::send_tick(net::server::Packet &sp) const
{
    sp.type = net::server::Packet::TICK;
    entity->world.get_entities_positions(sp.tick.entities); //TODO
    sp.tick.player_pos = entity->get_pos();
}

bool Player::send_signal(net::server::Packet &sp)
{
    if(!sent_init)
    {
        util::log("PLAYER", util::INFO, "initializing to client");
        sp.type = net::server::Packet::INIT;
        sp.init.conf.tick_rate = conf.tick_rate; //TODO Player::prepare_conf?
        std::copy(conf.server_name.cbegin(), conf.server_name.cend(),
                  std::back_inserter(sp.init.server_name));
        sp.init.chunk_size = CHK_SIZE;
        sent_init = true;
        return true;
    }
    else
    {
        if(send_chunks(sp)) return true;
    }
    return false;
}

bool Player::send_chunks(net::server::Packet &sp)
{
    bool is_sending = false;
    ChkPos iter;
    ChkPos center = to_chk_pos(glm::round(entity->get_pos()));
    for(iter.z = center.z - load_range;
        iter.z <= center.z + load_range;
        ++iter.z)
    {
        for(iter.y = center.y - load_range;
            iter.y <= center.y + load_range;
            ++iter.y)
        {
            for(iter.x = center.x - load_range;
                iter.x <= center.x + load_range;
                ++iter.x)
            {
                if(loaded_chunks.count(iter) == 0)
                {
                    if(glm::distance((Vec3F)iter, (Vec3F)center)
                           <= load_range)
                    {
                        entity->world.guarantee_chunk(iter);
                        auto const &chunk =
                            entity->world.get_chunk(iter);
                        send_chunk(sp, chunk, iter);
                        loaded_chunks.insert(iter);
                        is_sending = true;
                    }
                }
            }
        }
    }
    return is_sending;
}

void Player::send_chunk(net::server::Packet &sp, Chunk const &world_chunk,
                        ChkPos const &pos)
{
    sp.type = net::server::Packet::MAP;
    auto &chunk = sp.map.chunks.emplace_back();
    chunk.pos = pos;
    //TODO prevent copying?
    std::copy(world_chunk.voxels.cbegin(), world_chunk.voxels.cend(),
              chunk.voxels.begin());
    std::copy(world_chunk.light_lvls.cbegin(), world_chunk.light_lvls.cend(),
              chunk.light_lvls.begin());
}

void Player::init_from_client(net::client::Init const &ci)
{
    util::log("PLAYER", util::INFO, "received initialization data");
    String client_name(ci.client_name.begin(), ci.client_name.end());
    util::log("PLAYER", util::INFO, "client name: %s", client_name);
    change_config(ci.conf);
}

void Player::change_config(net::client::Conf const &cc)
{
    util::log("PLAYER", util::INFO, "changing config");
    util::log("PLAYER", util::INFO, "load range: %.2f", cc.load_range);
    load_range = cc.load_range;
}

Entity &Player::get_entity()
{
    return *entity;
}
