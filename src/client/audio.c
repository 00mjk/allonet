#include "_client.h"
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include "../util.h"

#define DEBUG_AUDIO 0

allo_media_track *media_find_track_by_id(alloclient_internal *cl, uint32_t track_id) {
    for(size_t i = 0; i < cl->media_tracks.length; i++) {
        allo_media_track *track = &cl->media_tracks.data[i];
        if (track->track_id == track_id) {
            return track;
        }
    }
    return NULL;
}

void _alloclient_parse_media(alloclient *client, unsigned char *data, size_t length)
{
    alloclient_internal *cl = _internal(client);
    
    // get the track_id from the top of data
    uint32_t track_id;
    assert(length >= sizeof(track_id) + 3);
    memcpy(&track_id, data, sizeof(track_id));
    track_id = ntohl(track_id);
    data += sizeof(track_id);
    length -= sizeof(track_id);
    
    // see if we have allocated a media object for this
    allo_media_track *track = media_find_track_by_id(cl, track_id);
    if (!track) {
        //TODO: track created in statediff is on other thread. workaround until state is shared across threads. 
        track = _allo_media_track_create(client, track_id, allo_media_type_audio);
    }
    
    if (track->type == allo_media_type_audio) {
        // todo: decode on another tread
        OpusDecoder *decoder = track->info.audio.decoder;
        FILE *debugFile = track->info.audio.debug;
        
        const int maximumFrameCount = 5760; // 120ms as per documentation
        int16_t *pcm = calloc(maximumFrameCount, sizeof(int16_t));
        int samples_decoded = opus_decode(decoder, (unsigned char*)data, length, pcm, maximumFrameCount, 0);

        assert(samples_decoded >= 0);
        if (debugFile) {
            fwrite(pcm, sizeof(int16_t), samples_decoded, debugFile);
            fflush(debugFile);
        }

        if(!client->audio_callback || client->audio_callback(client, track_id, pcm, samples_decoded)) {
            free(pcm);
        }
    } else {
        
    }
}

extern allo_media_track *_allo_media_track_find(alloclient *client, uint32_t track_id, allo_media_track_type type) {
    allo_media_track *track = media_find_track_by_id(_internal(client), track_id);
    if (track) {
        assert(track->type == type);
    }
    return track;
}

allo_media_track *_allo_media_track_create(alloclient *client, uint32_t track_id, allo_media_track_type type) {
    alloclient_internal *cl = _internal(client);
    // reserve 1 extra and use that
    arr_reserve(&cl->media_tracks, cl->media_tracks.length+1);
    allo_media_track *track = &cl->media_tracks.data[cl->media_tracks.length++];
    track->track_id = track_id;
    track->type = type;
    if (type == allo_media_type_audio) {
        int err;
        track->info.audio.decoder = opus_decoder_create(48000, 1, &err);
        if (DEBUG_AUDIO) {
            char name[255]; snprintf(name, 254, "track_%04d.pcm", track_id);
            track->info.audio.debug = fopen(name, "wb");
            fprintf(stderr, "Opening decoder for %s\n", name);
        } else {
            track->info.audio.debug = NULL;
        }
        assert(track->info.audio.decoder);
    }
    return track;
}

void _alloclient_media_destroy(alloclient *client, uint32_t track_id)
{
    allo_media_track *track = media_find_track_by_id(_internal(client), track_id);
    alloclient_internal *cl = _internal(client);
    if (track == NULL) {
        fprintf(stderr, "A decoder for track_id %d was not found\n", track_id);
        fprintf(stderr, "Active decoder tracks:\n ");
        track = NULL;
        for (size_t i = 0; i < cl->media_tracks.length; i++) {
            allo_media_track *track = &cl->media_tracks.data[i];
            fprintf(stderr, "%d, ", track->track_id);
        }
        return;
    }
    if (track->type == allo_media_type_audio) {
        assert(track->info.audio.decoder);

        if (DEBUG_AUDIO) {
            char name[255]; snprintf(name, 254, "track_%04d.pcm", track_id);
            fprintf(stderr, "Closing decoder for %s\n", name);
            if (track->info.audio.debug) {
                fclose(track->info.audio.debug);
                track->info.audio.debug = NULL;
            }
        }
        opus_decoder_destroy(track->info.audio.decoder);
        track->info.audio.decoder = NULL;
    } else {
        
    }
    for (size_t i = 0; i < cl->media_tracks.length; i++) {
        if (cl->media_tracks.data[i].track_id == track_id) {
            arr_splice(&_internal(client)->media_tracks, i, 1);
            break;
        }
    }
}


void _alloclient_send_audio(alloclient *client, int32_t track_id, const int16_t *pcm, size_t frameCount)
{
    assert(frameCount == 480 || frameCount == 960);
    
    if (_internal(client)->peer == NULL) {
        fprintf(stderr, "alloclient: Skipping send audio as we don't even have a peer\n");
        return;
    }
    
    if (_internal(client)->peer->state != ENET_PEER_STATE_CONNECTED) {
        fprintf(stderr, "alloclient: Skipping send audio as peer is not connected\n");
        return;
    }
    
    const int headerlen = sizeof(int32_t); // track id header
    const int outlen = headerlen + frameCount*2 + 1; // theoretical max
    ENetPacket *packet = enet_packet_create(NULL, outlen, 0 /* unreliable */);
    assert(packet != NULL);
    int32_t big_track_id = htonl(track_id);
    memcpy(packet->data, &big_track_id, headerlen);

    int len = opus_encode (
        _internal(client)->opus_encoder, 
        pcm, frameCount,
        packet->data + headerlen, outlen - headerlen
    );

    if (len < 3) {  // error or DTX ("do not transmit")
        enet_packet_destroy(packet);
        if (len < 0) {
            fprintf(stderr, "Error encoding audio to send: %d", len);
        }
        return;
    }
    // +1 because stupid server code assumes all packets end with a newline... FIX THE DAMN PROTOCOL
    int ok = enet_packet_resize(packet, headerlen + len+1);
    assert(ok == 0); (void)ok;
    ok = enet_peer_send(_internal(client)->peer, CHANNEL_MEDIA, packet);
    allo_statistics.bytes_sent[0] += packet->dataLength;
    allo_statistics.bytes_sent[1+CHANNEL_MEDIA] += packet->dataLength;
    assert(ok == 0);
}
