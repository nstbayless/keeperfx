/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file net_game.c
 *     Network game support for Dungeon Keeper.
 * @par Purpose:
 *     Functions to exchange packets through network.
 * @par Comment:
 *     None.
 * @author   KeeperFX Team
 * @date     11 Mar 2010 - 09 Oct 2010
 * @par  Copying and copyrights:
 *     This program is free software; you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation; either version 2 of the License, or
 *     (at your option) any later version.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "net_matchmaking.h"
#include "net_game.h"

#include "globals.h"
#include "bflib_basics.h"
#include "bflib_coroutine.h"
#include "bflib_datetm.h"
#include "bflib_enet.h"
#include "net_exchange_common.h"
#include "net_lobby.h"
#include "net_main.h"
#include "net_resync.h"

#include "player_data.h"
#include "player_instances.h"
#include "local_camera.h"
#include "front_landview.h"
#include "player_utils.h"
#include "packets.h"
#include "frontend.h"
#include "front_network.h"
#include "config_settings.h"
#include "config_keeperfx.h"
#include "config_strings.h"
#include "custom_sprites.h"
#include "dungeon_data.h"
#include "game_legacy.h"
#include "gui_msgs.h"
#include "net_exchange_gameplay.h"
#include "net_input_lag.h"
#include "net_checksums.h"
#include "keeperfx.hpp"
#include "post_inc.h"

#ifdef __cplusplus
extern "C" {
#endif
/******************************************************************************/
struct TbNetworkPlayerInfo net_player_info[MAX_NET_USERS];
extern int32_t multiplayer_speed_adjustment_ns;
/******************************************************************************/

#pragma pack(1)
struct StartupSyncPacket {
    uint8_t startup_sync_packet_valid;
    int32_t video_rotate_mode;
    TbBigChecksum map_checksums[NETWORK_STARTUP_MAP_FILE_COUNT];
    TbBigChecksum required_sprite_zip_checksums[REQUIRED_SPRITE_ZIP_COUNT];
    uint16_t initial_tendencies;
    uint32_t isometric_view_zoom_level;
    uint32_t frontview_zoom_level;
    uint32_t zoom_distance_setting;
    uint32_t frontview_zoom_distance_setting;
    uint8_t initial_input_lag_turns;
    uint8_t requested_slot;
};
#pragma pack()

short setup_network_service(enum FrontendNetService service)
{
  struct ServiceInitData *init_data = NULL;
  SYNCMSG("Initializing 4-players type %d network", service);
  memset(net_player_info, 0, sizeof(net_player_info));
  network_lobby_ping = 0;
  if (service != FrontendNetSvc_Online && service != FrontendNetSvc_LAN) {
    process_network_error(-800);
    return 0;
  }
  if ( LbNetwork_Init(NS_ENET_UDP, MAX_NET_USERS, &net_player_info[0], init_data) )
  {
    process_network_error(-800);
    return 0;
  }
  net_service_index_selected = service;
  if (service == FrontendNetSvc_LAN) {
    frontend_button_info[11].capstr_idx = GUIStr_MnuLanLobby;
    frontend_button_info[12].capstr_idx = GUIStr_MnuLanLobbies;
  } else {
    frontend_button_info[11].capstr_idx = GUIStr_MnuOnlineLobby;
    frontend_button_info[12].capstr_idx = GUIStr_MnuOnlineLobbies;
  }
  frontend_set_state(FeSt_NET_SESSION);
  return 1;
}

unsigned long get_host_player_id(void) {
    return 0;
}

static void setup_local_player_number(void);

/** Sentinel in StartupSyncPacket.requested_slot meaning "assign me automatically". */
#define NET_SLOT_AUTO 0xFF

/* Network user id -> player slot.
 *
 * Derived from the exchanged startup sync packets, so every machine computes the same
 * table. Kept out of struct Game deliberately: that struct is written verbatim to savegames
 * behind a size check (game_saves.c), so it cannot take new fields for state that is
 * re-derived at the start of every match anyway. */
static PlayerNumber s_net_user_slot[MAX_NET_USERS];

void reset_net_user_slots(void)
{
    for (NetUserId id = 0; id < MAX_NET_USERS; id++) {
        s_net_user_slot[id] = -1;
    }
}

PlayerNumber net_user_player_slot(NetUserId id)
{
    if ((id < 0) || (id >= MAX_NET_USERS)) {
        return -1;
    }
    return s_net_user_slot[id];
}

/** True when two or more network users drive this player slot ("archon"). */
TbBool net_slot_is_shared(PlayerNumber slot)
{
    int count = 0;
    for (NetUserId id = 0; id < MAX_NET_USERS; id++) {
        if (s_net_user_slot[id] == slot) {
            count++;
        }
    }
    return (count > 1);
}

/** Lowest user driving this slot, or -1. The one that speaks for the slot where it must
 *  still act as a single player; the host whenever the host is on it, since host == 0. */
NetUserId net_slot_primary_user(PlayerNumber slot)
{
    for (NetUserId id = 0; id < MAX_NET_USERS; id++) {
        if (s_net_user_slot[id] == slot) {
            return id;
        }
    }
    return -1;
}

/** Lowest still-connected user driving this slot, or -1 once all of them have left. */
NetUserId net_slot_active_user(PlayerNumber slot)
{
    for (NetUserId id = 0; id < MAX_NET_USERS; id++) {
        if ((s_net_user_slot[id] == slot) && network_player_active(id)) {
            return id;
        }
    }
    return -1;
}

/** The player slot the host occupies. The host is always SERVER_ID, but that no longer
 *  implies player slot 0 once slots can be requested. */
PlayerNumber net_host_player_slot(void)
{
    PlayerNumber slot = net_user_player_slot(SERVER_ID);
    return (slot >= 0) ? slot : (PlayerNumber)get_host_player_id();
}

/** Can a network user occupy this player slot?
 *
 * Multiplayer is limited to the four keeper slots. PLAYER_GOOD would additionally need the
 * PlaF_CompCtrl carve-out that init_players_local_game() applies, PLAYER_NEUTRAL can never
 * win, and no shipped map has start positions past PLAYERS = 4. */
static TbBool net_slot_is_selectable(long slot)
{
    return (slot >= PLAYER0) && (slot <= PLAYER3);
}

/** Build the user -> slot table from the exchanged startup sync packets.
 *
 * Every machine runs this over identical data in ascending user order, so all of them agree
 * without any extra protocol. Users that asked for a slot get it; the rest fill the lowest
 * unclaimed slots, which reproduces the old join-order behaviour exactly when nobody asks. */
static void resolve_net_user_slots(const struct StartupSyncPacket startup_sync_packets[MAX_NET_USERS])
{
    TbBool slot_claimed[PLAYERS_COUNT] = {false};
    reset_net_user_slots();

    for (NetUserId id = 0; id < MAX_NET_USERS; id++) {
        if (!net_player_info[id].network_user_active) {
            continue;
        }
        long wanted = startup_sync_packets[id].requested_slot;
        if (wanted == NET_SLOT_AUTO) {
            continue;
        }
        if (!net_slot_is_selectable(wanted)) {
            WARNLOG("User %d cannot have player slot %ld, assigning automatically", (int)id, wanted);
            continue;
        }
        s_net_user_slot[id] = (PlayerNumber)wanted;
        slot_claimed[wanted] = true;
    }

    for (NetUserId id = 0; id < MAX_NET_USERS; id++) {
        if (!net_player_info[id].network_user_active || (s_net_user_slot[id] >= 0)) {
            continue;
        }
        for (long slot = PLAYER0; slot <= PLAYER3; slot++) {
            if (!slot_claimed[slot]) {
                s_net_user_slot[id] = (PlayerNumber)slot;
                slot_claimed[slot] = true;
                break;
            }
        }
        if (s_net_user_slot[id] < 0) {
            ERRORLOG("No free player slot for network user %d", (int)id);
        }
    }

    for (NetUserId id = 0; id < MAX_NET_USERS; id++) {
        if (net_player_info[id].network_user_active) {
            NETLOG("Network user %d -> player slot %d%s", (int)id, (int)s_net_user_slot[id],
                net_slot_is_shared(s_net_user_slot[id]) ? " (shared)" : "");
        }
    }
}

TbBool network_is_host(void)
{
    return (netstate.my_id == SERVER_ID);
}

int setup_old_network_service(void)
{
    return setup_network_service(net_service_index_selected);
}


static void setup_players_from_startup_packets(const struct StartupSyncPacket startup_sync_packets[MAX_NET_USERS])
{
    for (int i = 0; i < MAX_NET_USERS; i++) {
        const struct StartupSyncPacket *sync = &startup_sync_packets[i];
        if (!net_player_info[i].network_user_active) {
            continue;
        }
        int k = s_net_user_slot[i];
        if (k < 0) {
            continue;
        }
        struct PlayerInfo *player = get_player(k);
        if (flag_is_set(player->allocflags, PlaF_Allocated)) {
            // Another user already built this slot; we share it. The slot's shared state
            // belongs to the first user, so only do the machine-local parts here.
            if (i == netstate.my_id) {
                init_player_local_view(player);
                init_local_cameras(player);
            }
            SYNCLOG("Network user %d shares player slot %d", i, k);
            continue;
        }
        player->id_number = k;
        player->primary_user_id = i;
        player->allocflags |= PlaF_Allocated;
        switch (sync->video_rotate_mode) {
            case 0: player->view_mode_restore = PVM_IsoWibbleView; break;
            case 1: player->view_mode_restore = PVM_IsoStraightView; break;
            case 2: player->view_mode_restore = PVM_FrontView; break;
            default: player->view_mode_restore = PVM_IsoWibbleView; break;
        }
        player->is_active = 1;
        init_player(player, 0);
        player->isometric_view_zoom_level = sync->isometric_view_zoom_level;
        player->frontview_zoom_level = sync->frontview_zoom_level;
        TbBool imprison = (sync->initial_tendencies & CrTend_Imprison) != 0;
        TbBool flee = (sync->initial_tendencies & CrTend_Flee) != 0;
        set_creature_tendencies(player, CrTend_Imprison, imprison);
        set_creature_tendencies(player, CrTend_Flee, flee);
        if (player->id_number == my_player_number) {
            game.creatures_tend_imprison = imprison;
            game.creatures_tend_flee = flee;
        }
        snprintf(player->player_name, sizeof(struct TbNetworkPlayerName), "%s", net_player[i].name);
    }
}

static TbBool verify_map_checksums(const struct StartupSyncPacket startup_sync_packets[MAX_NET_USERS])
{
    const TbBigChecksum *host = startup_sync_packets[SERVER_ID].map_checksums;
    for (int i = 0; i < MAX_NET_USERS; i++) {
        const TbBigChecksum *client = startup_sync_packets[i].map_checksums;
        if (!net_player_info[i].network_user_active) {
            continue;
        }
        int diff_count = 0;
        for (int j = 0; j < NETWORK_STARTUP_MAP_FILE_COUNT; j++) {
            if (client[j] == host[j]) {
                continue;
            }
            if (diff_count == 0) {
                ERRORLOG("Level checksums differ for player %d", i);
            }
            ERRORLOG("Level file map%05u.%s differs for player %d", get_loaded_level_number(), network_startup_compare_files[j], i);
            diff_count++;
        }
        if (diff_count != 0) {
            return false;
        }
    }
    NETLOG("Map checksums are verified");
    return true;
}

static TbBool verify_startup_sprite_zip_checksums(const struct StartupSyncPacket startup_sync_packets[MAX_NET_USERS])
{
    const NetUserId host_player_id = SERVER_ID;
    const struct StartupSyncPacket *host_sync = &startup_sync_packets[host_player_id];
    TbBool verified = true;
    for (int i = 0; i < MAX_NET_USERS; i++) {
        if (!net_player_info[i].network_user_active || i == host_player_id) {
            continue;
        }
        const struct StartupSyncPacket *client_sync = &startup_sync_packets[i];
        for (int zip_idx = 0; zip_idx < REQUIRED_SPRITE_ZIP_COUNT; zip_idx++) {
            if (client_sync->required_sprite_zip_checksums[zip_idx] == host_sync->required_sprite_zip_checksums[zip_idx]) {
                continue;
            }
            WARNLOG("Custom sprite zip differs between %s and %s: %s", network_player_name(host_player_id), network_player_name(i), required_sprite_zips[zip_idx]);
            message_add_fmt(MsgType_Blank, 0, "/fxdata/%.30s differs for %.12s", required_sprite_zips[zip_idx], network_player_name(i));
            verified = false;
        }
    }
    return verified;
}

static struct StartupSyncPacket s_local_startup_sync;
static struct StartupSyncPacket s_startup_sync_packets[MAX_NET_USERS];
static TbBool network_disconnect_victory_enabled;

static uint8_t calculate_initial_input_lag(void)
{
    int32_t player_count = 0;
    for (int32_t i = 0; i < MAX_NET_USERS; i++) {
        if (net_player_info[i].network_user_active) {
            player_count++;
        }
    }
    uint64_t ping = network_lobby_ping;
    if (player_count == 2) {
        ping /= 2;
    }
    uint64_t input_lag_turns = 0;
    if (turns_per_second > 0) {
        input_lag_turns = (ping * turns_per_second + 999) / 1000;
    }
    uint64_t uncapped_input_lag_turns = input_lag_turns;
    if (input_lag_turns > 0) {
        input_lag_turns -= 1;
    }
    if (input_lag_turns > MAXIMUM_INPUT_LAG_TURNS) {
        input_lag_turns = MAXIMUM_INPUT_LAG_TURNS;
    }
    JUSTLOG("Initial input lag: (%llu ms * %d turns/s + 999) / 1000 = %llu turns, adjusted to %llu", (unsigned long long)ping, turns_per_second, (unsigned long long)uncapped_input_lag_turns, (unsigned long long)input_lag_turns);
    return input_lag_turns;
}

static void build_local_startup_sync(void)
{
    memset(&s_local_startup_sync, 0, sizeof(s_local_startup_sync));
    s_local_startup_sync.startup_sync_packet_valid = 1;
    s_local_startup_sync.video_rotate_mode = settings.video_rotate_mode;
    calculate_network_startup_map_checksums(s_local_startup_sync.map_checksums);
    memcpy(s_local_startup_sync.required_sprite_zip_checksums, required_sprite_zip_checksums, sizeof(s_local_startup_sync.required_sprite_zip_checksums));
    uint16_t initial_tendencies = 0;
    if (IMPRISON_BUTTON_DEFAULT) {initial_tendencies |= CrTend_Imprison;}
    if (FLEE_BUTTON_DEFAULT) {initial_tendencies |= CrTend_Flee;}
    s_local_startup_sync.initial_tendencies = initial_tendencies;
    s_local_startup_sync.isometric_view_zoom_level = settings.isometric_view_zoom_level;
    s_local_startup_sync.frontview_zoom_level = settings.frontview_zoom_level;
    s_local_startup_sync.zoom_distance_setting = zoom_distance_setting;
    s_local_startup_sync.frontview_zoom_distance_setting = frontview_zoom_distance_setting;
    s_local_startup_sync.initial_input_lag_turns = calculate_initial_input_lag();
    // -human names the player slot we want; force_player_num says it was given explicitly.
    s_local_startup_sync.requested_slot = force_player_num ? (uint8_t)default_loc_player : NET_SLOT_AUTO;
}

static TbBool net_startup_sync_exchange_and_apply(void)
{
    memset(s_startup_sync_packets, 0, sizeof(s_startup_sync_packets));
    if (exchange_frame_block(NETMSG_STARTUP_SYNC, &s_local_startup_sync, s_startup_sync_packets, sizeof(struct StartupSyncPacket)) != Lb_OK) {
        ERRORLOG("Startup sync exchange failed");
        return false;
    }

    for (int i = 0; i < MAX_NET_USERS; i++) {
        if (net_player_info[i].network_user_active && !s_startup_sync_packets[i].startup_sync_packet_valid) {
            ERRORLOG("Startup sync exchange missed one or more peers");
            return false;
        }
    }
    if (!verify_map_checksums(s_startup_sync_packets)) {
        create_frontend_error_box(5000, get_string(GUIStr_NetUnsyncedMap));
        return false;
    }

    if (!verify_startup_sprite_zip_checksums(s_startup_sync_packets)) {
        create_frontend_error_box(5000, get_string(GUIStr_NetVerifyFxdataSame));
        return false;
    }
    const struct StartupSyncPacket *host_sync = &s_startup_sync_packets[SERVER_ID];
    game.input_lag_turns = host_sync->initial_input_lag_turns;
    input_lag_reset();
    game.skip_initial_input_turns = calculate_skip_input();
    NETLOG("Startup input lag: %d", game.input_lag_turns);
    zoom_distance_setting = host_sync->zoom_distance_setting;
    frontview_zoom_distance_setting = host_sync->frontview_zoom_distance_setting;
    resolve_net_user_slots(s_startup_sync_packets);
    // Must precede player setup: init_player() and friends branch on is_my_player(), which
    // is meaningless while my_player_number still holds our net user id.
    setup_local_player_number();
    setup_players_from_startup_packets(s_startup_sync_packets);
    return true;
}

/** Switch my_player_number from the net user id it carries during the lobby to the player
 *  slot this machine drives.
 *
 * Cannot run earlier: the lobby and land view index net_screen_packet[] by user id while
 * comparing against my_player_number, so the two must stay equal until the match starts. */
static void setup_local_player_number(void)
{
    PlayerNumber slot = net_user_player_slot(netstate.my_id);
    if (slot < 0) {
        ERRORLOG("Local player number %d not found among active network players", my_player_number);
        return;
    }
    my_player_number = slot;
    game.local_plyr_idx = slot;
    SYNCLOG("Local player is net user %d, player slot %d", (int)netstate.my_id, (int)slot);
}

void setup_count_players(void)
{
  if (game.game_kind == GKind_LocalGame)
  {
    game.active_players_count = 1;
  } else
  {
    TbBool slot_counted[PLAYERS_COUNT] = {false};
    game.active_players_count = 0;
    for (int i = 0; i < MAX_NET_USERS; i++)
    {
      if (!net_player_info[i].network_user_active)
        continue;
      // Several users may share one slot; the slot counts once.
      PlayerNumber slot = net_user_player_slot(i);
      if (slot < 0) {
        game.active_players_count++;
      } else if (!slot_counted[slot]) {
        slot_counted[slot] = true;
        game.active_players_count++;
      }
    }
  }
}

TbBool init_players_network_game(void)
{
    SYNCDBG(4,"Starting");
    TbBool initialized = true;
    for (int zip_idx = 0; zip_idx < REQUIRED_SPRITE_ZIP_COUNT; zip_idx++) {
        if (required_sprite_zip_checksums[zip_idx] != 0) {
            continue;
        }
        WARNLOG("Required custom sprite zip missing: %s", required_sprite_zips[zip_idx]);
        message_add_fmt(MsgType_Blank, 0, "/fxdata/%.30s missing", required_sprite_zips[zip_idx]);
        create_frontend_error_box(5000, get_string(GUIStr_NetVerifyFxdataSame));
        initialized = false;
        break;
    }
    if (initialized) {
        build_local_startup_sync();
        initialized = net_startup_sync_exchange_and_apply();
    }
    if (initialized && netstate.my_id == SERVER_ID && frontnet_service_selected(FrontendNetSvc_Online)) {
        LevelNumber map_number = get_level_number();
        struct LevelInformation *level_info = get_level_info(map_number);
        const char *map_name = "";
        if (level_info) {
            map_name = level_info->name;
            if (level_info->name_stridx > 0) {
                map_name = get_string(level_info->name_stridx);
            }
        }
        matchmaking_finish_lobby(MMLobbyResult_Started, (int)map_number, map_name);
    }
    if (!initialized) {
        LbNetwork_Stop();
    }
    return initialized;
}

void are_disconnect_victories_allowed(void)
{
    struct PlayerInfo *myplyr = get_my_player();
    network_disconnect_victory_enabled = false;
    for (int player_index = 0; player_index < game.active_players_count; player_index++) {
        struct PlayerInfo *player = get_player(player_index);
        if (player_exists(player) && !is_my_player(player) && players_are_enemies(myplyr->id_number, player->id_number)) {
            network_disconnect_victory_enabled = true;
            return;
        }
    }
}

/** Check whether a network player is active.
 *
 * @param plyr_idx
 * @return
 */
TbBool network_player_active(int plyr_idx)
{
    if ((plyr_idx < 0) || (plyr_idx >= MAX_NET_USERS))
        return false;
    return (net_player_info[plyr_idx].network_user_active != 0);
}

const char *network_player_name(int plyr_idx)
{
    if ((plyr_idx < 0) || (plyr_idx >= MAX_NET_USERS))
        return NULL;
    return net_player_info[plyr_idx].name;
}

static void resolve_network_quit_outcome(struct PlayerInfo *player)
{
    if (player->victory_state != VicS_Undecided) {
        return;
    }
    if (player_cannot_win(player->id_number)) {
        set_player_as_lost_level(player);
        return;
    }
    set_player_as_won_level(player);
}

static TbBool network_has_remote_enemies_remaining(void)
{
    struct PlayerInfo *myplyr = get_my_player();
    for (int i = 0; i < PLAYERS_COUNT; i++) {
        struct PlayerInfo *player = get_player(i);
        TbBool is_active_enemy = player_exists(player) && !is_my_player(player) && player->is_active == 1 && !player_cannot_win(player->id_number) && players_are_enemies(myplyr->id_number, player->id_number);
        TbBool is_connected_network_player = (player->allocflags & PlaF_CompCtrl) == 0 && network_player_active(player->primary_user_id);
        TbBool is_initial_computer_player = (player->allocflags & PlaF_CompCtrl) != 0 && i >= game.active_players_count;
        if (is_active_enemy && (is_connected_network_player || is_initial_computer_player)) {
            return true;
        }
    }
    return false;
}

TbBool network_human_contenders_remain(void)
{
    for (PlayerNumber player_idx = 0; player_idx < PLAYERS_COUNT; player_idx++) {
        struct PlayerInfo *player = get_player(player_idx);
        if (player_exists(player) && (player->is_active == 1) && ((player->allocflags & PlaF_CompCtrl) == 0) && !player_cannot_win(player_idx)) {
            return true;
        }
    }
    return false;
}

static TbBool network_has_remote_users_remaining(void)
{
    for (NetUserId user_id = 0; user_id < (NetUserId)netstate.max_players; user_id += 1) {
        if (user_id != netstate.my_id && netstate.users[user_id].progress != USER_UNUSED) {
            return true;
        }
    }
    return false;
}

static void replace_network_player_with_ai(struct PlayerInfo *player)
{
    player->allocflags |= PlaF_CompCtrl;
    toggle_computer_player(player->id_number);
    message_add(MsgType_Player, player->id_number, get_string(GUIStr_NetAiTookOver));
    JUSTLOG("p:%d computer took over", player->id_number);
}

static void stop_network_game_state(void)
{
    memset(net_player_info, 0, sizeof(net_player_info));
    clear_flag(game.system_flags, GSF_NetworkActive);
    clear_flag(game.system_flags, GSF_NetGameNoSync);
    clear_flag(game.system_flags, GSF_NetSeedNoSync);
    fe_network_active = 0;
    game.game_kind = GKind_LocalGame;
    game.input_lag_turns = 0;
    game.skip_initial_input_turns = 0;
    input_lag_reset();
    multiplayer_speed_adjustment_ns = 0;
    setup_count_players();
}

static void stop_network_game_and_quit_to_main_menu(void)
{
    LbNetwork_Stop();
    stop_network_game_state();
    quit_game = 1;
}

static void stop_network_game_and_continue_locally(void)
{
    LbNetwork_Stop();
    stop_network_game_state();
    get_my_player()->display_objective_turn = get_gameturn() + 1;
}

static TbBool host_already_won_level(void)
{
    GameTurn newest_turn = get_gameturn();
    for (GameTurnDelta offset = 0; offset <= game.input_lag_turns; offset += 1) {
        if ((GameTurn)offset > newest_turn) {
            break;
        }
        const struct Packet *host_packet = get_history_packet(SERVER_ID, newest_turn - offset);
        if (host_packet != NULL && host_packet->action == PckA_FinishGame && host_packet->actn_par1 == VicS_WonLevel) {
            return true;
        }
    }
    return false;
}

void process_player_leave_game_packet(struct PlayerInfo *player)
{
    if (player != get_my_player()) {
        if (network_is_active()) {
            OnDroppedUser(player->primary_user_id, NETDROP_MANUAL);
            process_disconnected_network_players();
            return;
        }
    } else if (network_is_active()) {
        stop_network_game_and_quit_to_main_menu();
    } else {
        quit_game = 1;
    }
    player->allocflags &= ~PlaF_Allocated;
}

void process_disconnected_network_players(void)
{
    if (!network_is_active()) {
        return;
    }
    struct PlayerInfo *myplyr = get_my_player();
    TbBool host_disconnected = (netstate.my_id != SERVER_ID) && (netstate.users[SERVER_ID].progress == USER_UNUSED);
    TbBool disconnected = host_disconnected;
    TbBool enemy_disconnected = false;
    TbBool winning_quit = false;
    int32_t plyr_count = 0;
    if (host_disconnected && host_already_won_level()) {
        myplyr->additional_flags &= ~PlaAF_UnlockedLordTorture;
        quit_game = 1;
        return;
    }
    for (int player_index = 0; player_index < MAX_NET_USERS; player_index++) {
        struct PlayerInfo *player = get_player(player_index);
        if (!player_exists(player) || is_my_player(player)) {
            continue;
        }
        // Only drop a slot once every user driving it has gone. If some remain but the one
        // named as primary is not among them, hand the slot to a survivor and play on.
        const NetUserId surviving_user = net_slot_active_user(player->id_number);
        if (!host_disconnected && (surviving_user >= 0)) {
            if (player->primary_user_id != (unsigned char)surviving_user) {
                JUSTLOG("p:%d primary user %d left; slot continues under user %d",
                    (int)player->id_number, (int)player->primary_user_id, (int)surviving_user);
                player->primary_user_id = (unsigned char)surviving_user;
            }
            continue;
        }
        disconnected = true;
        if (network_disconnect_victory_enabled && players_are_enemies(myplyr->id_number, player->id_number)) {
            enemy_disconnected = true;
            if (!winning_quit && winning_player_quitting(player, &plyr_count)) {
                winning_quit = true;
            }
        }
        if ((player->allocflags & PlaF_CompCtrl) == 0) {
            network_lobby_ping = GetPing(netstate.my_id);
            input_lag_reset_request(calculate_initial_input_lag());
            if (!host_disconnected && player->id_number != get_host_player_id() && player->player_name[0] != '\0') {
                message_add_fmt(MsgType_Blank, 0, get_string(GUIStr_NetPlayerDisconnected), player->player_name);
                JUSTLOG("p:%d player %s departed", player->id_number, player->player_name);
            }
            if (player->victory_state == VicS_Undecided) {
                replace_network_player_with_ai(player);
                continue;
            }
        }
        if (player->victory_state != VicS_Undecided) {
            player->allocflags &= ~PlaF_Allocated;
        }
    }

    TbBool has_enemies_to_defeat = network_has_remote_enemies_remaining();
    if (!disconnected || (!host_disconnected && has_enemies_to_defeat)) {
        return;
    }
    if (host_disconnected) {
        message_add(MsgType_Blank, 0, get_string(GUIStr_NetHostConnectionLost));
        if (has_enemies_to_defeat) {
            stop_network_game_and_continue_locally();
            return;
        }
    }
    if (winning_quit) {
        for (int i = 0; i < PLAYERS_COUNT; i++) {
            struct PlayerInfo *swplyr = get_player(i);
            if (player_exists(swplyr) && (swplyr->is_active == 1)) {
                resolve_network_quit_outcome(swplyr);
            }
        }
    }
    if (enemy_disconnected) {
        resolve_network_quit_outcome(myplyr);
    }
    if (winning_quit && (plyr_count > 1)) {
        if (game.conf.rules[myplyr->id_number].gameplay.winner_tortures_loser) {
            myplyr->additional_flags |= PlaAF_UnlockedLordTorture;
        } else {
            myplyr->additional_flags &= ~PlaAF_UnlockedLordTorture;
        }
    }
    if (!host_disconnected && network_has_remote_users_remaining()) {
        return;
    }
    if (enemy_disconnected && myplyr->victory_state == VicS_Undecided) {
        stop_network_game_and_quit_to_main_menu();
    } else {
        stop_network_game_and_continue_locally();
    }
}

long network_session_join(void)
{
    int32_t plyr_num;
    reset_attempting_to_join_cancel();
    display_attempting_to_join_message(-1);
    if (attempting_to_join_cancel_requested())
        return -1;
    snprintf(join_lobby_id, sizeof(join_lobby_id), "%s", net_session[net_session_index_active]->join_address);
    if (LbNetwork_Join(net_session[net_session_index_active], net_player_name, &plyr_num, NULL) == 0)
        return plyr_num;
    join_lobby_id[0] = '\0';
    if (!attempting_to_join_cancel_requested()) {
        if (frontnet_service_selected(FrontendNetSvc_Online)) {
            net_session_index_active = -1;
            net_session_index_active_id = -1;
            matchmaking_request_list();
        }
        process_network_error(-802);
    }
    return -1;
}

void sync_initial_network_seed(void)
{
   if (!network_is_active()) {
      return;
   }
   if (!LbNetwork_Resync(&game.action_random_seed, sizeof(game.action_random_seed))) {
      ERRORLOG("Initial sync failed");
      return;
   }
   game.ai_random_seed = game.action_random_seed * 9377 + 9391;
   game.player_random_seed = game.action_random_seed * 9473 + 9479;
   NETLOG("Initial network seed synced: action_seed=%u", game.action_random_seed);
}
/******************************************************************************/
#ifdef __cplusplus
}
#endif
