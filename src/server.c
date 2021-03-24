#include <allonet/server.h>
#include <enet/enet.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <assert.h>
#include <cJSON/cJSON.h>
#include "util.h"
#include <allonet/arr.h>
#include "asset.h"
#include <allonet/assetstore.h>

void allo_send(alloserver *serv, alloserver_client *client, allochannel channel, const uint8_t *buf, int len);

typedef arr_t(ENetPeer *) PeerList;
// When a peer requests an asset that we do not have in cache
typedef struct {
    char *id; // asset id
    ENetPeer *peer;  // THe peer that wants the asset
    ENetPeer *source;// the peer we have choosen to download the asset from
} wanted_asset;

typedef struct {
    ENetHost *enet;
    /// map from asset_id to list of client peers
    arr_t(wanted_asset*) wanted_assets;
    assetstore assetstore;
} alloserv_internal;

typedef struct {
    ENetPeer *peer;
} alloserv_client_internal;

static alloserv_internal *_servinternal(alloserver *serv)
{
    return (alloserv_internal*)serv->_internal;
}

static alloserv_client_internal *_clientinternal(alloserver_client *client)
{
    return (alloserv_client_internal*)client->_internal;
}

static alloserver_client *_client_create()
{
    alloserver_client *client = (alloserver_client*)calloc(1, sizeof(alloserver_client));
    client->_internal = (void*)calloc(1, sizeof(alloserv_client_internal));
    for(int i = 0; i < AGENT_ID_LENGTH; i++)
    {
        client->agent_id[i] = 'a' + rand() % 25;
    }
    client->agent_id[AGENT_ID_LENGTH] = '\0';
    client->intent = allo_client_intent_create();
    _clientinternal(client)->peer = NULL;

    return client;
}

static void alloserv_client_free(alloserver_client *client)
{
    allo_client_intent_free(client->intent);
    free(_clientinternal(client));
    free(client);
}

static void handle_incoming_connection(alloserver *serv, ENetPeer* new_peer)
{
    alloserver_client *new_client = _client_create();
    char host[255] = {0};
    enet_address_get_host_ip(&new_peer->address, host, 254);
    printf ("A new client connected from %s:%u as %s/%p.\n", 
        host,
        new_peer->address.port,
        new_client->agent_id,
        (void*)new_client
    );
    
    _clientinternal(new_client)->peer = new_peer;
    _clientinternal(new_client)->peer->data = (void*)new_client;
    LIST_INSERT_HEAD(&serv->clients, new_client, pointers);

    // very hard timeout limits; change once clients actually send SYN
    enet_peer_timeout(new_peer, 0, 5000, 5000);
    if(serv->clients_callback) {
        serv->clients_callback(serv, new_client, NULL);
    }
}

wanted_asset *_asset_is_wanted(const char *asset_id, alloserver *server);
void _add_asset_to_wanted(const char *asset_id, alloserver *server, alloserver_client *client);
void _request_missing_asset(alloserver *server, alloserver_client *client, const char *asset_id);
void _remove_asset_from_wanted(const char *asset_id, alloserver *server, alloserver_client *client);
void _forward_wanted_asset(const char *asset_id, alloserver *server, alloserver_client *client);
void _forward_wanted_asset_failure(const char *asset_id, alloserver *server, alloserver_client *client);
void _remove_client_from_wanted(alloserver *server, alloserver_client *client);

typedef struct asset_user {
    alloserver *server;
    alloserver_client *client;
    ENetPeer *peer;
} asset_user;


/// @param user must be an asset_user
static void _asset_send_func_broadcast(asset_mid mid, const cJSON *header, const uint8_t *data, size_t data_length, void *user) {
    // Build packet and send to everyone except the client in user
    
    alloserver *server = ((asset_user *)user)->server;
    alloserver_client *client = ((asset_user *)user)->client;
    
    ENetPacket *packet = asset_build_enet_packet(mid, header, data, data_length);
    
    alloserver_client *other;
    LIST_FOREACH(other, &server->clients, pointers) {
        if (other == client) continue;
        ENetPeer *peer = _clientinternal(other)->peer;
        
        enet_peer_send(peer, CHANNEL_ASSETS, packet);
        allo_statistics.bytes_sent[0] += packet->dataLength;
        allo_statistics.bytes_sent[1+CHANNEL_ASSETS] += packet->dataLength;
    }
}
void _asset_send_func_peer(asset_mid mid, const cJSON *header, const uint8_t *data, size_t data_length, void *user) {
    ENetPeer *peer = ((asset_user *)user)->peer;
    
    asset_packet_header packet_header;
    char *cheader = cJSON_Print(header);
    packet_header.mid = mid;
    packet_header.hlen = strlen(cheader);
    free((void*)cheader);
    
    ENetPacket *packet = asset_build_enet_packet(mid, header, data, data_length);
    enet_peer_send(peer, CHANNEL_ASSETS, packet);
    allo_statistics.bytes_sent[0] += packet->dataLength;
    allo_statistics.bytes_sent[1+CHANNEL_ASSETS] += packet->dataLength;
}
void _asset_send_func(asset_mid mid, const cJSON *header, const uint8_t *data, size_t data_length, void *user) {
    // Extract peer if one is not already set.
    // If user->peer is set it's probably for a destination other than user->client
    ENetPeer *peer = ((asset_user *)user)->peer;
    if (peer == NULL) {
        alloserver_client *client = ((asset_user *)user)->client;
        peer = _clientinternal(client)->peer;
    }
    asset_user usr = *(asset_user*)user;
    usr.peer = peer;
    
    _asset_send_func_peer(mid, header, data, data_length, &usr);
}

static void _asset_request_bytes_func(const char *asset_id, size_t offset, size_t length, void *user) {
    alloserver *server = ((asset_user *)user)->server;
    alloserver_client *client = ((asset_user *)user)->client;
    
    uint8_t *buffer = malloc(length);
    assert(buffer);
    
    size_t total_size = 0;
    int read_length = assetstore_read(&(_servinternal(server)->assetstore), asset_id, offset, buffer, length, &total_size);
    
    if (read_length > 0) {
        asset_deliver_bytes(asset_id, buffer, offset, read_length, total_size, _asset_send_func, user);
    } else {
        _request_missing_asset(server, client, asset_id);
    }
    free(buffer);
}

static int _asset_write_func(const char *asset_id, const uint8_t *buffer, size_t offset, size_t length, size_t total_size, void *user) {
    alloserver *server = ((asset_user *)user)->server;
    alloserver_client *sender = ((asset_user *)user)->client;
    ENetPeer *sender_peer = _clientinternal(sender)->peer;
    wanted_asset *wanted = _asset_is_wanted(asset_id, server);
    
    if (wanted && (wanted->source == 0 || wanted->source == sender_peer)) {
        wanted->source = sender_peer;
        return assetstore_write(&(_servinternal(server)->assetstore), asset_id, offset, buffer, length, total_size);
    } else {
        return 0;
    }
}

static void _asset_state_callback_func(const char *asset_id, asset_state state, void *user) {
    alloserver *server = ((asset_user *)user)->server;
    alloserver_client *client = ((asset_user *)user)->client;
    
    if (state == asset_state_now_available) {
        printf("Forwarding asset %s\n", asset_id);
        // asset was completed
        // Ping registered peers by sending a first chunk.
        _forward_wanted_asset(asset_id, server, client);
    } else if (state == asset_state_now_unavailable) {
        _forward_wanted_asset_failure(asset_id, server, client);
    } else {
        printf("Unhandled asset state %d\n", state);
    }
}

static void handle_assets(const uint8_t *data, size_t data_length, alloserver *server, alloserver_client *client) {
    
    asset_user usr = { .server = server, .client = client };
    asset_handle(data, data_length, _asset_request_bytes_func, _asset_write_func, _asset_send_func, _asset_state_callback_func, (void*)&usr);
}

static void handle_incoming_data(alloserver *serv, alloserver_client *client, allochannel channel, ENetPacket *packet)
{
    allo_statistics.bytes_recv[0] += packet->dataLength;
    if (channel < CHANNEL_COUNT) {
        allo_statistics.bytes_recv[1+channel] += packet->dataLength;
    }
    
    if(channel == CHANNEL_MEDIA)
    {
        alloserver_client *other;
        LIST_FOREACH(other, &serv->clients, pointers)
        {
            if(other == client) continue;

            allo_send(serv, other, CHANNEL_MEDIA, packet->data, packet->dataLength);
        }
        return;
    } else if (channel == CHANNEL_ASSETS) {
        handle_assets(packet->data, packet->dataLength, serv, client);
        return;
    }
    
    
    if(serv->raw_indata_callback && channel != CHANNEL_ASSETS)
    {
        serv->raw_indata_callback(
            serv, 
            client, 
            channel, 
            packet->data,
            packet->dataLength
        );
    }
}

static void handle_lost_connection(alloserver *serv, alloserver_client *client)
{
    char host[255] = {0};
    ENetPeer *peer = _clientinternal(client)->peer;
    enet_address_get_host_ip(&peer->address, host, 254);
    printf("%s/%p from %s:%d disconnected.\n", client->agent_id, (void*)client, host, peer->address.port);

    // scan through the list of asset->peers and remove the peer where peeresent
    _remove_client_from_wanted(serv, client);
    
    LIST_REMOVE(client, pointers);
    peer->data = NULL;
    if(serv->clients_callback) {
        serv->clients_callback(serv, NULL, client);
    }
    alloserv_client_free(client);
}

static bool allo_poll(alloserver *serv, int timeout)
{
    ENetEvent event;
    enet_host_service (_servinternal(serv)->enet, &event, timeout);
    alloserver_client *client = event.peer ? (alloserver_client*)event.peer->data : NULL;

    switch (event.type)
    {
        case ENET_EVENT_TYPE_CONNECT:
            handle_incoming_connection(serv, event.peer);
            break;
    
        case ENET_EVENT_TYPE_RECEIVE:
            if (client == NULL) {
                // old data from disconnected client?!
                break;
            }
            handle_incoming_data(serv, client, event.channelID, event.packet);
            enet_packet_destroy (event.packet);
            break;
    
        case ENET_EVENT_TYPE_DISCONNECT:
            handle_lost_connection(serv, client);
            break;

        case ENET_EVENT_TYPE_NONE:
            return false;
    }
    return true;
}


void allo_send(alloserver *serv, alloserver_client *client, allochannel channel, const uint8_t *buf, int len)
{
    (void)serv;
    ENetPacket *packet = enet_packet_create(
        NULL,
        len,
        (channel==CHANNEL_COMMANDS || channel==CHANNEL_ASSETS) ?
            ENET_PACKET_FLAG_RELIABLE :
            0
    );
    memcpy(packet->data, buf, len);
    enet_peer_send(_clientinternal(client)->peer, channel, packet);
    allo_statistics.bytes_sent[0] += packet->dataLength;
    if (channel < CHANNEL_COUNT) {
        allo_statistics.bytes_sent[1+channel] += packet->dataLength;
    }
}

void alloserv_send_enet(alloserver *serv, alloserver_client *client, allochannel channel, ENetPacket *packet)
{
    (void)serv;
    enet_peer_send(_clientinternal(client)->peer, channel, packet);
    allo_statistics.bytes_sent[0] += packet->dataLength;
    if (channel < CHANNEL_COUNT) {
        allo_statistics.bytes_sent[1+channel] += packet->dataLength;
    }
}

alloserver *allo_listen(int listenhost, int port)
{
    alloserver *serv = (alloserver*)calloc(1, sizeof(alloserver));
    serv->_internal = (alloserv_internal*)calloc(1, sizeof(alloserv_internal));
    arr_init(&_servinternal(serv)->wanted_assets);
    
    assetstore *assetstore = &(_servinternal(serv)->assetstore);
    asset_memstore_init(assetstore);
    asset_memstore_register_asset_nocopy(assetstore, "hello", (uint8_t*)"Hello World!", 13);
    
    srand((unsigned int)time(NULL));

    ENetAddress address;
    address.host = listenhost;
    address.port = port;
    char printable[255] = {0};
    enet_address_get_host_ip(&address, printable, 254);
    printf("Alloserv attempting listen on %s:%d...\n", printable, port);
    _servinternal(serv)->enet = enet_host_create(
        &address,
        allo_client_count_max,
        CHANNEL_COUNT,
        0,  // no ingress bandwidth limit
        0   // no egress bandwidth limit
    );
    if (_servinternal(serv)->enet == NULL)
    {
        fprintf (
            stderr, 
             "An error occurred while trying to create an ENet server host.\n"
        );
        alloserv_stop(serv);
        return NULL;
    }

    serv->_port = _servinternal(serv)->enet->address.port;
    serv->interbeat = allo_poll;
    serv->send = allo_send;
    LIST_INIT(&serv->clients);
    LIST_INIT(&serv->state.entities);
    
    return serv;
}

void alloserv_disconnect(alloserver *serv, alloserver_client *client, int reason_code)
{
    (void)serv;
    enet_peer_disconnect_later(_clientinternal(client)->peer, reason_code);
}

void alloserv_stop(alloserver* serv)
{
  enet_host_destroy(_servinternal(serv)->enet);
  free(_servinternal(serv));
  free(serv);
}

int allo_socket_for_select(alloserver *serv)
{
    return _servinternal(serv)->enet->socket;
}

wanted_asset *_asset_is_wanted(const char *asset_id, alloserver *server) {
    alloserv_internal *sv = _servinternal(server);
    for (size_t i = 0; i < sv->wanted_assets.length; i++) {
        wanted_asset *wanted = sv->wanted_assets.data[i];
        if (strcmp(wanted->id, asset_id) == 0) {
            return wanted;
        }
    }
    return NULL;
}

void _add_asset_to_wanted(const char *asset_id, alloserver *server, alloserver_client *client) {
    alloserv_internal *sv = _servinternal(server);
    
    wanted_asset *wanted = malloc(sizeof(wanted_asset));
    wanted->id = strdup(asset_id);
    wanted->peer = _clientinternal(client)->peer;
    wanted->source = NULL;
    arr_push(&sv->wanted_assets, wanted);
}

void _request_missing_asset(alloserver *server, alloserver_client *client, const char *asset_id) {
    int already_wanted = _asset_is_wanted(asset_id, server) != NULL;
    _add_asset_to_wanted(asset_id, server, client);
    
    // Broadcast request the asset unless it is already wanted
    if (!already_wanted) {
        asset_user usr = { .server = server, .client = client };
        asset_request(asset_id, NULL, _asset_send_func_broadcast, &usr);
    }
}

void _remove_asset_from_wanted(const char *asset_id, alloserver *server, alloserver_client *client) {
    alloserv_internal *sv = _servinternal(server);
    ENetPeer *peer = _clientinternal(client)->peer;
    
    for (size_t i = 0; i < sv->wanted_assets.length; i++) {
        wanted_asset *wanted = sv->wanted_assets.data[i];
        if (peer == wanted->peer && strcmp(wanted->id, asset_id) == 0) {
            arr_splice(&sv->wanted_assets, i, 1);
            --i;
            free(wanted->id);
            free(wanted);
        }
    }
}

void _forward_wanted_asset_failure(const char *asset_id, alloserver *server, alloserver_client *client) {
    alloserv_internal *sv = _servinternal(server);
    for (size_t i = 0; i < sv->wanted_assets.length; i++) {
        wanted_asset *wanted = sv->wanted_assets.data[i];
        if (strcmp(wanted->id, asset_id) == 0) {
            // deliver the first bytes. After this it's up to the client to request more.
            asset_user usr = { .server = server, .client = client, .peer = wanted->peer };
            asset_deliver_error(asset_id, asset_not_available_error, "Server could not find asset", _asset_send_func, &usr);
            
            arr_splice(&sv->wanted_assets, i, 1);
            --i;
            free(wanted->id);
            free(wanted);
        }
    }
}

void _forward_wanted_asset(const char *asset_id, alloserver *server, alloserver_client *client) {
    alloserv_internal *sv = _servinternal(server);
    
    for (size_t i = 0; i < sv->wanted_assets.length; i++) {
        wanted_asset *wanted = sv->wanted_assets.data[i];
        if (strcmp(wanted->id, asset_id) == 0) {
            //TODO: Setup job to send to only send to a few peers at once.
            // deliver the first bytes. After this it's up to the client to request more.
            // TODO: only deliver the range requested in the original request
            asset_user usr = { .server = server, .client = client, .peer = wanted->peer };
            asset_deliver(asset_id, _asset_request_bytes_func, &usr);
            
            arr_splice(&sv->wanted_assets, i, 1);
            --i;
            free(wanted->id);
            free(wanted);
        }
    }
}

void _remove_client_from_wanted(alloserver *server, alloserver_client *client) {
    alloserv_internal *sv = _servinternal(server);
    ENetPeer *peer = _clientinternal(client)->peer;
    
    for (size_t i = 0; i < sv->wanted_assets.length; i++) {
        wanted_asset *wanted = sv->wanted_assets.data[i];
        if (wanted->peer == peer) {
            arr_splice(&sv->wanted_assets, i, 1);
            --i;
            free(wanted->id);
            free(wanted);
        }
    }
}
