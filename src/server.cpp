#include <cstring>
#include <algorithm>
//
#include <enet/enet.h>
//
#include <lux_shared/common.hpp>
#include <lux_shared/net/common.hpp>
#include <lux_shared/net/data.hpp>
#include <lux_shared/net/enet.hpp>
#include <lux_shared/net/serial.hpp>
//
#include <map.hpp>
#include <entity.hpp>
#include <command.hpp>
#include "server.hpp"

Uns constexpr MAX_CLIENTS  = 16;

struct Server {
    F64 tick_rate = 0.0;
    struct Client {
        ENetPeer* peer;
        String    name;
        Entity*   entity;
        VecSet<ChkPos> loaded_chunks;
        bool      admin = false;
    };
    DynArr<Client> clients;

    bool is_running = false;

    ENetHost*  host;
} server;

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

    { ///kick all
        LUX_LOG("kicking all clients");
        auto it = server.clients.begin();
        while(it != server.clients.end()) {
            kick_client(it->name.c_str(), "server stopping");
            it = server.clients.begin();
        }
    }
    enet_host_destroy(server.host);
    enet_deinitialize();
}

bool is_client_connected(Uns id) {
    return id < server.clients.size();
}

void erase_client(Uns id) {
    LUX_ASSERT(is_client_connected(id));
    LUX_LOG("client disconnected");
    LUX_LOG("    id: %zu" , id);
    LUX_LOG("    name: %s", server.clients[id].name.c_str());
    remove_entity(*server.clients[id].entity);
    server.clients.erase(server.clients.begin() + id);
}

void kick_peer(ENetPeer *peer) {
    U8* ip = get_ip(peer->address);
    LUX_LOG("terminating connection with peer");
    LUX_LOG("    ip: %u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    enet_peer_disconnect_now(peer, 0);
}

LUX_MAY_FAIL static server_send_msg(Server::Client& client,
                                    char const* beg, SizeT len) {
    char constexpr prefix[] = "[SERVER]: ";
    SizeT total_len = (sizeof(prefix) - 1) + len;
    SizeT pack_sz = sizeof(NetSsSgnl::Header) +
        sizeof(NetSsSgnl::Msg) + total_len;
    ENetPacket* out_pack;
    if(create_reliable_pack(out_pack, pack_sz) != LUX_OK) {
        return LUX_FAIL;
    }
    U8* iter = out_pack->data;
    serialize(&iter, (U8 const&)NetSsSgnl::MSG);
    serialize(&iter, (U32 const&)total_len);
    serialize(&iter, prefix, sizeof(prefix) - 1);
    serialize(&iter, beg, len);
    LUX_ASSERT(iter == out_pack->data + out_pack->dataLength);
    return send_packet(client.peer, out_pack, SIGNAL_CHANNEL);
}

void kick_client(char const* name, char const* reason) {
    LUX_LOG("kicking client");
    LUX_LOG("    name: %s", name);
    LUX_LOG("    reason: %s", reason);
    String s_name(name);
    auto it = std::find_if(server.clients.begin(), server.clients.end(),
        [&] (Server::Client const& v) { return v.name == s_name; });
    Uns client_id = it - server.clients.begin();
    LUX_LOG("    id: %zu", client_id);
    if(!is_client_connected(client_id)) {
        LUX_LOG("tried to kick non-existant client");
        return; //@CONSIDER return value for failure
    }
    String msg = String("you got kicked for: ") + String(reason);
    (void)server_send_msg(server.clients[client_id], msg.c_str(), msg.size());
    enet_host_flush(server.host);
    kick_peer(server.clients[client_id].peer);
    erase_client(client_id);
}

LUX_MAY_FAIL add_client(ENetPeer* peer) {
    U8* ip = get_ip(peer->address);
    LUX_LOG("new client connecting")
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
        U8 const* iter = in_pack->data;
        if(check_pack_size(sizeof(NetCsInit), iter, in_pack) != LUX_OK) {
            return LUX_FAIL;
        }

        deserialize(&iter, &cs_init.net_ver.major);
        deserialize(&iter, &cs_init.net_ver.minor);
        deserialize(&iter, &cs_init.net_ver.patch);
        deserialize(&iter, &cs_init.name);
        LUX_ASSERT(iter == in_pack->data + in_pack->dataLength);

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
    }

    { ///send init packet
        ENetPacket* out_pack;
        if(create_reliable_pack(out_pack, sizeof(NetSsInit)) != LUX_OK) {
            return LUX_FAIL;
        }
        U8* iter = out_pack->data;

        U8 constexpr server_name[] = "lux-server";
        static_assert(sizeof(server_name) <= SERVER_NAME_LEN);
        Arr<char, SERVER_NAME_LEN> buff;
        std::memcpy(buff, server_name, sizeof(server_name));
        std::memset(buff + sizeof(server_name), 0,
                    SERVER_NAME_LEN - sizeof(server_name));
        U16 tick_rate = server.tick_rate;
        serialize(&iter, buff);
        serialize(&iter, tick_rate);
        LUX_ASSERT(iter == out_pack->data + out_pack->dataLength);

        if(send_packet(peer, out_pack, INIT_CHANNEL) != LUX_OK) return LUX_FAIL;
    }

    Server::Client& client = server.clients.emplace_back();
    client.peer = peer;
    client.peer->data = (void*)(server.clients.size() - 1);
    client.name = String((char const*)cs_init.name);
    client.entity = &create_player();

    LUX_LOG("client connected successfully");
    LUX_LOG("    name: %s", client.name.c_str());
#ifndef NDEBUG
    (void)server_make_admin(client.name.c_str());
#endif
    return LUX_OK;
}

LUX_MAY_FAIL send_map_load(ENetPeer* peer, Slice<ChkPos> const& requests) {
    typedef NetSsSgnl::MapLoad::Chunk NetChunk;
    SizeT pack_sz = sizeof(NetSsSgnl::Header) + sizeof(NetSsSgnl::MapLoad) +
        requests.len * sizeof(NetChunk);

    ENetPacket* out_pack;
    if(create_reliable_pack(out_pack, pack_sz) != LUX_OK) {
        return LUX_FAIL;
    }

    U8* iter = out_pack->data;
    serialize(&iter, (U8 const&)NetSsSgnl::MAP_LOAD);
    serialize(&iter, (U32 const&)requests.len);
    for(Uns i = 0; i < requests.len; ++i) {
        guarantee_chunk(requests[i]);
        Chunk const& chunk = get_chunk(requests[i]);

        serialize(&iter, requests[i]);
        serialize(&iter, chunk.voxels);
        serialize(&iter, chunk.light_lvls);
    }
    LUX_ASSERT(iter == out_pack->data + out_pack->dataLength);
    if(send_packet(peer, out_pack, SIGNAL_CHANNEL) != LUX_OK) return LUX_FAIL;

    ///we need to do it outside, because we must be sure that the packet has
    ///been received (i.e. sent in this case, enet guarantees that the peer will
    ///either receive it or get disconnected
    Server::Client& client = server.clients[(Uns)peer->data];
    for(Uns i = 0; i < requests.len; ++i) {
        client.loaded_chunks.emplace(requests[i]);
    }
    return LUX_OK;
}

//@TODO use slice
LUX_MAY_FAIL send_light_update(ENetPeer* peer, DynArr<ChkPos> const& updates) {
    Server::Client& client = server.clients[(Uns)peer->data];
    SizeT output_len = 0;
    for(Uns i = 0; i < updates.size(); ++i) {
        ChkPos const& pos = updates[i];
        if(client.loaded_chunks.count(pos) > 0) {
            ++output_len;
        }
    }
    if(output_len == 0) return LUX_OK;
    typedef NetSsSgnl::LightUpdate::Chunk NetChunk;
    SizeT pack_sz = sizeof(NetSsSgnl::Header) + sizeof(NetSsSgnl::LightUpdate) +
        output_len * sizeof(NetChunk);

    ENetPacket* out_pack;
    if(create_reliable_pack(out_pack, pack_sz) != LUX_OK) {
        return LUX_FAIL;
    }

    U8* iter = out_pack->data;
    serialize(&iter, (U8 const&)NetSsSgnl::LIGHT_UPDATE);
    serialize(&iter, (U32 const&)output_len);
    for(Uns i = 0; i < updates.size(); ++i) {
        ChkPos const& pos = updates[i];
        if(client.loaded_chunks.count(pos) > 0) {
            Chunk const& chunk = get_chunk(pos);
            serialize(&iter, pos);
            serialize(&iter, chunk.light_lvls);
        }
    }
    LUX_ASSERT(iter == out_pack->data + out_pack->dataLength);
    if(send_packet(peer, out_pack, SIGNAL_CHANNEL) != LUX_OK) return LUX_FAIL;
    return LUX_OK;
}

LUX_MAY_FAIL handle_tick(ENetPeer* peer, ENetPacket *in_pack) {
    U8 const* iter = in_pack->data;
    if(check_pack_size(sizeof(NetCsTick), iter, in_pack) != LUX_OK) {
        return LUX_FAIL;
    }

    LUX_ASSERT(is_client_connected((Uns)peer->data));
    Entity& entity = *server.clients[(Uns)peer->data].entity;
    Vec2F player_dir;
    deserialize(&iter, &player_dir);
    LUX_ASSERT(iter == in_pack->data + in_pack->dataLength);
    if(player_dir.x != 0.f || player_dir.y != 0.f) {
        player_dir = glm::normalize(player_dir);
        entity.vel.x = player_dir.x * 0.1f;
        entity.vel.y = player_dir.y * 0.1f;
    }
    return LUX_OK;
}

LUX_MAY_FAIL handle_signal(ENetPeer* peer, ENetPacket* in_pack) {
    U8 const* iter = in_pack->data;
    if(check_pack_size_atleast(sizeof(NetCsSgnl::Header), iter, in_pack)
           != LUX_OK) {
        LUX_LOG("couldn't read signal header");
        return LUX_FAIL;
    }

    NetCsSgnl sgnl;
    deserialize(&iter, (U8*)&sgnl.header);

    if(sgnl.header >= NetCsSgnl::HEADER_MAX) {
        LUX_LOG("unexpected signal header %u", sgnl.header);
        return LUX_FAIL;
    }

    SizeT expected_stt_sz;
    switch(sgnl.header) {
        case NetCsSgnl::MAP_REQUEST: {
            expected_stt_sz = sizeof(NetCsSgnl::MapRequest);
        } break;
        case NetCsSgnl::COMMAND: {
            expected_stt_sz = sizeof(NetCsSgnl::Command);
        } break;
        default: LUX_UNREACHABLE();
    }
    if(check_pack_size_atleast(expected_stt_sz, iter, in_pack) != LUX_OK) {
        LUX_LOG("couldn't read static segment");
        return LUX_FAIL;
    }

    SizeT expected_dyn_sz;
    switch(sgnl.header) {
        case NetCsSgnl::MAP_REQUEST: {
            deserialize(&iter, &sgnl.map_request.requests.len);
            expected_dyn_sz = sgnl.map_request.requests.len * sizeof(ChkPos);
        } break;
        case NetCsSgnl::COMMAND: {
            deserialize(&iter, &sgnl.command.contents.len);
            expected_dyn_sz = sgnl.command.contents.len;
        } break;
        default: LUX_UNREACHABLE();
    }
    if(check_pack_size(expected_dyn_sz, iter, in_pack) != LUX_OK) {
        LUX_LOG("couldn't read dynamic segment");
        return LUX_FAIL;
    }

    switch(sgnl.header) {
        case NetCsSgnl::MAP_REQUEST: {
            Slice<ChkPos> requests;
            requests.len = sgnl.map_request.requests.len;
            requests.beg = lux_alloc<ChkPos>(requests.len);
            LUX_DEFER { lux_free(requests.beg); };

            deserialize(&iter, &requests.beg, requests.len);

            //@CONSIDER, should we really fail here? perhaps split the func
            if(send_map_load(peer, requests) != LUX_OK) return LUX_FAIL;
        } break;
        case NetCsSgnl::COMMAND: {
            //@CONSIDER denying it on an earlier stage
            Slice<char> command;
            command.len = sgnl.command.contents.len;
            command.beg = lux_alloc<char>(command.len);
            LUX_DEFER { lux_free(command.beg); };

            deserialize(&iter, &command.beg, command.len);
            Uns client_id = (Uns)peer->data;
            if(!server.clients[client_id].admin) {
                //@TODO send msg
                LUX_LOG("client %s tried to execute command \"%s\""
                        " without admin rights",
                        server.clients[client_id].name.c_str(), command.beg);
                char constexpr DENY_MSG[] = "you do not have admin rights, this"
                    " incident will be reported";
                (void)server_send_msg(server.clients[client_id], DENY_MSG,
                                sizeof(DENY_MSG));
                return LUX_FAIL;
            }
            LUX_LOG("[%s]: %s", server.clients[client_id].name.c_str(),
                    command.beg);

            //@TODO we should redirect output somehow
            add_command(command.beg);
        } break;
        default: LUX_UNREACHABLE();
    }
    return LUX_OK;
}

void server_tick(DynArr<ChkPos> const& light_updated_chunks) {
    for(Server::Client& client : server.clients) {
        (void)send_light_update(client.peer, light_updated_chunks);
    }
    { ///handle events
        //@RESEARCH can we use our own packet to prevent copies?
        //@CONSIDER splitting this scope
        ENetEvent event;
        while(enet_host_service(server.host, &event, 0) > 0) {
            if(event.type == ENET_EVENT_TYPE_CONNECT) {
                if(add_client(event.peer) != LUX_OK) {
                    kick_peer(event.peer);
                }
            } else if(event.type == ENET_EVENT_TYPE_DISCONNECT) {
                erase_client((Uns)event.peer->data);
            } else if(event.type == ENET_EVENT_TYPE_RECEIVE) {
                LUX_DEFER { enet_packet_destroy(event.packet); };
                if(!is_client_connected((Uns)event.peer->data)) {
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
                            continue;
                        }
                    } else {
                        auto const &name =
                            server.clients[(Uns)event.peer->data].name;
                        LUX_LOG("ignoring unexpected packet");
                        LUX_LOG("    channel: %u", event.channelID);
                        LUX_LOG("    from: %s", name.c_str());
                    }
                }
            }
        }
    }

    { ///dispatch ticks
        for(Server::Client& client : server.clients) {
            ENetPacket* out_pack;
            if(create_unreliable_pack(out_pack, sizeof(NetSsTick)) != LUX_OK) {
                continue;
            }
            U8* iter = out_pack->data;
            serialize(&iter, client.entity->pos);
            LUX_ASSERT(iter == out_pack->data + out_pack->dataLength);
            (void)send_packet(client.peer, out_pack, TICK_CHANNEL);
        }
    }
}

void server_broadcast(char const* beg) {
    char const* end = beg;
    while(*end != '\0') ++end;
    ///we count the null terminator
    ++end;
    SizeT len = end - beg;
    for(Server::Client& client : server.clients) {
        (void)server_send_msg(client, beg, len);
    }
}

bool server_is_running() {
    return server.is_running;
}

void server_quit() {
    server.is_running = false;
}

LUX_MAY_FAIL server_make_admin(char const* name) {
    LUX_LOG("making %s an admin", name);
    String s_name(name);
    auto it = std::find_if(server.clients.begin(), server.clients.end(),
        [&] (Server::Client const& v) { return v.name == s_name; });
    Uns client_id = it - server.clients.begin();
    if(!is_client_connected(client_id)) {
        LUX_LOG("client %s is not connected", name);
        return LUX_FAIL;
    }
    server.clients[client_id].admin = true;
    return LUX_OK;
}
