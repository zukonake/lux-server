#include <cstring>
#include <algorithm>
//
#include <enet/enet.h>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/fast_square_root.hpp>
#include <glm/gtx/rotate_vector.hpp> //@TODO
//
#include <lux_shared/common.hpp>
#include <lux_shared/net/common.hpp>
#include <lux_shared/net/serial.hpp>
#include <lux_shared/net/data.hpp>
#include <lux_shared/net/data.inl>
#include <lux_shared/net/enet.hpp>
//
#include <map.hpp>
#include <entity.hpp>
#include "server.hpp"

Uns constexpr MAX_CLIENTS  = 16;

struct Server {
    F64 tick_rate = 0.0;
    struct Client {
        ENetPeer*      peer;
        StrBuff        name;
        EntityId       entity;
        VecSet<ChkPos> loaded_chunks;
        bool           admin = false;
    };
    SparseDynArr<Client> clients;

    bool is_running = false;

    ENetHost*  host;
} server;

NetCsSgnl cs_sgnl;
NetCsTick cs_tick;
NetSsInit ss_init;
NetSsSgnl ss_sgnl;
NetSsTick ss_tick;

void server_init(U16 port, F64 tick_rate) {
    server.tick_rate = tick_rate;

    LUX_LOG("initializing server");
    if(enet_initialize() != 0) {
        LUX_FATAL("couldn't initialize ENet");
    }

    {
        ENetAddress addr = {ENET_HOST_ANY, port};
        server.host = enet_host_create(&addr, MAX_CLIENTS, CHANNEL_NUM, 0, 0);
        if(server.host == nullptr) {
            LUX_FATAL("couldn't initialize ENet host");
        }
    }
    server.is_running = true;
}

void server_deinit() {
    server.is_running = false;
    LUX_LOG("deinitializing server");

    LUX_LOG("kicking all clients");
    for(auto pair : server.clients) {
        kick_client(pair.k, "server stopping"_l);
    }
    enet_host_destroy(server.host);
    enet_deinitialize();
}

bool is_client_connected(ClientId id) {
    return server.clients.contains(id);
}

void erase_client(ClientId id) {
    LUX_ASSERT(is_client_connected(id));
    LUX_LOG("client disconnected");
    LUX_LOG("    id: %zu" , id);
    auto const& name = server.clients[id].name;
    LUX_LOG("    name: %.*s", (int)name.len, name.beg);
    remove_entity(server.clients[id].entity);
    server.clients.erase(id);
}

void kick_peer(ENetPeer *peer) {
    U8* ip = get_ip(peer->address);
    LUX_LOG("terminating connection with peer");
    LUX_LOG("    ip: %u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    enet_peer_disconnect_now(peer, 0);
}

LUX_MAY_FAIL static server_send_msg(ClientId id, Str str) {
    Str constexpr prefix = "[SERVER]: "_l;
    ss_sgnl.tag = NetSsSgnl::MSG;
    ss_sgnl.msg.contents.resize(prefix.len + str.len);
    ss_sgnl.msg.contents.cpy(prefix).cpy(str);
    LUX_ASSERT(is_client_connected(id));
    return send_net_data(server.clients[id].peer, &ss_sgnl, SIGNAL_CHANNEL);
}

void kick_client(ClientId id, Str reason) {
    LUX_ASSERT(is_client_connected(id));
    DynStr name = (DynStr)server.clients[id].name;
    LUX_LOG("kicking client");
    LUX_LOG("    name: %.*s", (int)name.len, name.beg);
    LUX_LOG("    reason: %.*s", (int)reason.len, reason.beg);
    LUX_LOG("    id: %zu", id);
    Str constexpr prefix = "you got kicked for: "_l;
    StrBuff msg(prefix.len + reason.len);
    msg.cpy(prefix).cpy(reason);
    (void)server_send_msg(id, msg);
    enet_host_flush(server.host);
    kick_peer(server.clients[id].peer);
    erase_client(id);
}

LUX_MAY_FAIL add_client(ENetPeer* peer) {
    U8* ip = get_ip(peer->address);
    LUX_LOG("new client connecting");
    LUX_LOG("    ip: %u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);

    ENetPacket* in_pack;

    { ///retrieve init packet
        LUX_LOG("awaiting init packet");
        Uns constexpr MAX_TRIES = 10;
        Uns constexpr TRY_TIME  = 50; ///in milliseconds

        //@CONSIDER sleeping in a separate thread, so the server cannot be
        //frozen by malicious joining, perhaps a different, sleep-free solution
        //could be used
        Uns tries = 0;
        U8  channel_id;
        do {
            enet_host_service(server.host, nullptr, TRY_TIME);
            in_pack = enet_peer_receive(peer, &channel_id);
            if(in_pack != nullptr) {
                if(channel_id == INIT_CHANNEL) {
                    break;
                } else {
                    LUX_LOG("ignoring unexpected packet");
                    LUX_LOG("    channel: %u", channel_id);
                    enet_packet_destroy(in_pack);
                }
            }
            if(tries >= MAX_TRIES) {
                LUX_LOG("client did not send an init packet");
                return LUX_FAIL;
            }
            ++tries;
        } while(true);
        LUX_LOG("received init packet after %zu/%zu tries", tries, MAX_TRIES);
    }
    ///we need to keep the packet around, because we read its contents directly
    ///through the NetCsInit struct pointer
    LUX_DEFER { enet_packet_destroy(in_pack); };

    NetCsInit cs_init;
    { ///parse client init packet
        LUX_RETHROW(deserialize_packet(in_pack, &cs_init),
            "failed to deserialize init packet from client");

        if(cs_init.net_ver.major != NET_VERSION_MAJOR) {
            LUX_LOG("client uses an incompatible major lux net api version");
            LUX_LOG("    ours: %u", NET_VERSION_MAJOR);
            LUX_LOG("    theirs: %u", cs_init.net_ver.major);
            return LUX_FAIL;
        }
        if(cs_init.net_ver.minor >  NET_VERSION_MINOR) {
            LUX_LOG("client uses a newer minor lux net api version");
            LUX_LOG("    ours: %u", NET_VERSION_MINOR);
            LUX_LOG("    theirs: %u", cs_init.net_ver.minor);
            return LUX_FAIL;
        }

        ClientId duplicate_id = get_client_id((Str)cs_init.name);
        if(duplicate_id != server.clients.end().id) {
            //@CONSIDER kicking the new one instead
            LUX_LOG("client already connected, kicking the old one");
            kick_client(duplicate_id, "double-join"_l);
        }
    }

    { ///send init packet
        Str constexpr name = "lux-server"_l;
        LUX_ASSERT(name.len <= SERVER_NAME_LEN);
        ((DynStr)ss_init.name).cpy(name).set('\0');
        ss_init.tick_rate = server.tick_rate;

        LUX_RETHROW(send_net_data(peer, &ss_init, INIT_CHANNEL),
            "failed to send init data to client");
    }

    ClientId id = server.clients.emplace();
    Server::Client& client = server.clients[id];
    client.peer = peer;
    client.peer->data = (void*)id;
    client.name = (Str)cs_init.name;
    client.entity = create_player();
    //@TODO
    entity_comps.container[client.entity].items =
        {create_item("item0"_l), create_item("item1"_l), create_item("item2"_l)};
    entity_comps.name[client.entity] = (Str)client.name;

    LUX_LOG("client connected successfully");
    LUX_LOG("    name: %.*s", (int)client.name.len, client.name.beg);
#ifndef NDEBUG
    (void)server_make_admin(id);
#endif
    return LUX_OK;
}

LUX_MAY_FAIL send_tiles(ENetPeer* peer, VecSet<ChkPos> const& requests) {
    ss_sgnl.tag = NetSsSgnl::TILES;
    for(auto const& pos : requests) {
        guarantee_chunk(pos);
        Chunk const& chunk = get_chunk(pos);
        std::memcpy(ss_sgnl.tiles.chunks[pos].floor,
                    chunk.floor, sizeof(chunk.floor));
        std::memcpy(ss_sgnl.tiles.chunks[pos].wall,
                    chunk.wall, sizeof(chunk.wall));
        std::memcpy(ss_sgnl.tiles.chunks[pos].roof,
                    chunk.roof, sizeof(chunk.roof));
    }

    LUX_RETHROW(send_net_data(peer, &ss_sgnl, SIGNAL_CHANNEL),
        "failed to send map load data to client");

    ///we need to do it outside, because we must be sure that the packet has
    ///been received (i.e. sent in this case, enet guarantees that the peer will
    ///either receive it or get disconnected
    Server::Client& client = server.clients[(ClientId)peer->data];
    for(auto const& pos : requests) {
        client.loaded_chunks.emplace(pos);
    }
    return LUX_OK;
}

LUX_MAY_FAIL send_light(ENetPeer* peer, VecSet<ChkPos> const& chunks) {
    ss_sgnl.tag = NetSsSgnl::LIGHT;

    Server::Client& client = server.clients[(ClientId)peer->data];
    for(auto const& pos : chunks) {
        if(client.loaded_chunks.count(pos) > 0) {
            Chunk const& chunk = get_chunk(pos);
            std::memcpy(ss_sgnl.light.chunks[pos].light_lvl,
                        chunk.light_lvl, CHK_VOL * sizeof(LightLvl));
        }
    }
    if(ss_sgnl.light.chunks.size() == 0) return LUX_OK;
    return send_net_data(peer, &ss_sgnl, SIGNAL_CHANNEL);
}

LUX_MAY_FAIL handle_tick(ENetPeer* peer, ENetPacket *in_pack) {
    LUX_ASSERT(is_client_connected((ClientId)peer->data));
    LUX_RETHROW(deserialize_packet(in_pack, &cs_tick),
        "failed to deserialize tick from client")

    auto& client = server.clients[(ClientId)peer->data];
    for(auto action : cs_tick.actions) {
        //@TODO check if size is correct
        /*std::memcpy(client.rasen.o, action.bytecode.data(),
                    action.bytecode.len * sizeof(U16));
        client.rasen.run();*/
        LUX_UNIMPLEMENTED();
    }
    return LUX_OK;
}

LUX_MAY_FAIL handle_signal(ENetPeer* peer, ENetPacket* in_pack) {
    NetCsSgnl sgnl;
    LUX_RETHROW(deserialize_packet(in_pack, &sgnl),
        "failed to deserialize signal from client");

    switch(sgnl.tag) {
        case NetCsSgnl::MAP_REQUEST: {
            (void)send_tiles(peer, sgnl.map_request.requests);
            (void)send_light(peer, sgnl.map_request.requests);
        } break;
        case NetCsSgnl::COMMAND: {
            ClientId client_id = (ClientId)peer->data;
            auto const& name = server.clients[client_id].name;
            auto const& contents = sgnl.command.contents;
            if(!server.clients[client_id].admin) {
                LUX_LOG("client %.*s tried to execute command \"%*s\""
                        " without admin rights", (int)name.len, name.beg,
                        (int)contents.len, contents.beg);
                (void)server_send_msg(client_id, "you do not have admin rights,"
                                      " this incident will be reported"_l);
                return LUX_FAIL;
            }
            LUX_LOG("[%.*s]: %.*s", (int)name.len, name.beg, (int)contents.len,
                    contents.beg);

            //@TODO we should redirect output somehow
            //@TODO add_command(sgnl.command.contents.data());
            LUX_UNIMPLEMENTED();
        } break;
        default: LUX_UNREACHABLE();
    }
    return LUX_OK;
}

void server_tick() {
    for(auto pair : server.clients) {
        //@TODO only send loaded updates
        (void)send_light(pair.v.peer, light_updated_chunks);
        (void)send_tiles(pair.v.peer, tile_updated_chunks);
    }
    { ///handle events
        //@CONSIDER splitting this scope
        ENetEvent event;
        while(enet_host_service(server.host, &event, 0) > 0) {
            if(event.type == ENET_EVENT_TYPE_CONNECT) {
                if(add_client(event.peer) != LUX_OK) {
                    kick_peer(event.peer);
                }
            } else if(event.type == ENET_EVENT_TYPE_DISCONNECT) {
                erase_client((ClientId)event.peer->data);
            } else if(event.type == ENET_EVENT_TYPE_RECEIVE) {
                LUX_DEFER { enet_packet_destroy(event.packet); };
                if(!is_client_connected((ClientId)event.peer->data)) {
                    U8 *ip = get_ip(event.peer->address);
                    LUX_LOG("ignoring packet from not connected peer");
                    LUX_LOG("    ip: %u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
                    enet_peer_reset(event.peer);
                } else {
                    if(event.channelID == TICK_CHANNEL) {
                        if(handle_tick(event.peer, event.packet) != LUX_OK) {
                            continue;
                        }
                    } else if(event.channelID == SIGNAL_CHANNEL) {
                        if(handle_signal(event.peer, event.packet) != LUX_OK) {
                            LUX_LOG("failed to handle signal from client");
                            kick_client((ClientId)event.peer->data,
                                        "corrupted signal packet"_l);
                            continue;
                        }
                    } else {
                        auto const &name =
                            server.clients[(ClientId)event.peer->data].name;
                        LUX_LOG("ignoring unexpected packet");
                        LUX_LOG("    channel: %u", event.channelID);
                        LUX_LOG("    from: %.*s", (int)name.len, name.beg);
                    }
                }
            }
        }
    }

    { ///dispatch ticks
        //@IMPROVE we might want to turn this into a differential transfer,
        //instead of resending the whole state all the time
        NetSsTick::EntityComps net_comps;
        get_net_entity_comps(&net_comps);
        ss_tick.day_cycle = day_cycle;
        ss_tick.entity_comps = net_comps;
        for(auto pair : entities) {
            ss_tick.entities.emplace(pair.k);
        }
        for(auto pair : server.clients) {
            auto const& client = pair.v;
            ss_tick.player_id = client.entity;
            /*EntityId bullet = create_entity();
            entity_comps.vel[bullet] = glm::rotate(Vec2F(0.f, -1.f),
                entity_comps.orientation.at(client.entity).angle);
            entity_comps.pos[bullet] = entity_comps.pos.at(client.entity) +
                entity_comps.vel.at(bullet);
            entity_comps.shape[bullet] = EntityComps::Shape
                {{.rect = {{0.05f, 0.05f}}}, .tag = EntityComps::Shape::RECT};
            entity_comps.visible[bullet] = {2};
            entity_comps.orientation[bullet] =
                {entity_comps.orientation.at(client.entity).angle};*/

            (void)send_net_data(client.peer, &ss_tick, TICK_CHANNEL, false);
        }
        clear_net_data(&ss_tick);
    }
    server.clients.free_slots();
}

void server_broadcast(Str msg) {
    for(auto pair : server.clients) {
        (void)server_send_msg(pair.k, msg);
    }
}

bool server_is_running() {
    return server.is_running;
}

void server_quit() {
    server.is_running = false;
}

ClientId get_client_id(Str name) {
    ClientId rval = server.clients.end().id;
    for(auto pair : server.clients) {
        auto const& client = pair.v;
        if((Str)client.name == name) {
            rval = pair.k;
            break;
        }
    }
    return rval;
}

void server_make_admin(ClientId id) {
    LUX_LOG("making client %zu an admin", id);
    LUX_ASSERT(is_client_connected(id));
    server.clients[id].admin = true;
}

void add_dbg_point(NetSsTick::DbgInf::Shape::Point const& val,
                   Vec4F col, bool border) {
    ss_tick.dbg_inf.shapes.emplace(NetSsTick::DbgInf::Shape{
        .tag = NetSsTick::DbgInf::Shape::POINT, .point = val,
        .col = col, .border = border});
}

void add_dbg_line(NetSsTick::DbgInf::Shape::Line const& val,
                   Vec4F col, bool border) {
    ss_tick.dbg_inf.shapes.emplace(NetSsTick::DbgInf::Shape{
        .tag = NetSsTick::DbgInf::Shape::LINE, .line = val,
        .col = col, .border = border});
}

void add_dbg_arrow(NetSsTick::DbgInf::Shape::Arrow const& val,
                   Vec4F col, bool border) {
    ss_tick.dbg_inf.shapes.emplace(NetSsTick::DbgInf::Shape{
        .tag = NetSsTick::DbgInf::Shape::ARROW, .arrow = val,
        .col = col, .border = border});
}

void add_dbg_cross(NetSsTick::DbgInf::Shape::Cross const& val,
                   Vec4F col, bool border) {
    ss_tick.dbg_inf.shapes.emplace(NetSsTick::DbgInf::Shape{
        .tag = NetSsTick::DbgInf::Shape::CROSS, .cross = val,
        .col = col, .border = border});
}

void add_dbg_sphere(NetSsTick::DbgInf::Shape::Sphere const& val,
                   Vec4F col, bool border) {
    ss_tick.dbg_inf.shapes.emplace(NetSsTick::DbgInf::Shape{
        .tag = NetSsTick::DbgInf::Shape::SPHERE, .sphere = val,
        .col = col, .border = border});
}

void add_dbg_triangle(NetSsTick::DbgInf::Shape::Triangle const& val,
                   Vec4F col, bool border) {
    ss_tick.dbg_inf.shapes.emplace(NetSsTick::DbgInf::Shape{
        .tag = NetSsTick::DbgInf::Shape::TRIANGLE, .triangle = val,
        .col = col, .border = border});
}

void add_dbg_rect(NetSsTick::DbgInf::Shape::Rect const& val,
                   Vec4F col, bool border) {
    ss_tick.dbg_inf.shapes.emplace(NetSsTick::DbgInf::Shape{
        .tag = NetSsTick::DbgInf::Shape::RECT, .rect = val,
        .col = col, .border = border});
}
