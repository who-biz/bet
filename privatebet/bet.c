/******************************************************************************
 * Copyright © 2014-2018 The SuperNET Developers.                             *
 *                                                                            *
 * See the AUTHORS, DEVELOPER-AGREEMENT and LICENSE files at                  *
 * the top-level directory of this distribution for the individual copyright  *
 * holder information and the developer policies on copyright and licensing.  *
 *                                                                            *
 * Unless otherwise agreed in a custom licensing agreement, no part of the    *
 * SuperNET software, including this file may be copied, modified, propagated *
 * or distributed except according to the terms contained in the LICENSE file *
 *                                                                            *
 * Removal or modification of this copyright notice is prohibited.            *
 *                                                                            *
 ******************************************************************************/
#include "bet.h"
#include "../includes/curl/curl.h"
#include "../log/macrologger.h"
#include "cards777.h"
#include "cashier.h"
#include "client.h"
#include "commands.h"
#include "common.h"
#include "gfshare.h"
#include "host.h"
#include "network.h"
#include "table.h"
#include "storage.h"

#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

struct privatebet_info *bet_player = NULL;
struct privatebet_vars *player_vars = NULL;

uint8_t sharenrs[256];
bits256 deckid;
bits256 playershares[CARDS777_MAXCARDS][CARDS777_MAXPLAYERS];
int32_t permis_d[CARDS777_MAXCARDS], permis_b[CARDS777_MAXCARDS];
bits256 v_hash[CARDS777_MAXCARDS][CARDS777_MAXCARDS];
bits256 g_hash[CARDS777_MAXPLAYERS][CARDS777_MAXCARDS];
struct enc_share *g_shares = NULL;
int32_t max_players = 2;
static const int32_t poker_deck_size = 52;

char *dealer_config_file = "./config/dealer_config.json";
char *notaries_file = "./config/notaries.json";

char table_id[65];

void bet_set_table_id()
{
	bits256 randval;
	memset(table_id, 0x00, sizeof(table_id));
	OS_randombytes(randval.bytes, sizeof(randval));
	bits256_str(table_id, randval);
	printf("%s::%d::table_id::%s\n", __FUNCTION__, __LINE__, table_id);
}

static void bet_cashier_client_initialize(char *node_ip, const int32_t port)
{
	int32_t subsock = -1, pushsock = -1;
	char bind_sub_addr[128], bind_push_addr[128];

	bet_tcp_sock_address(0, bind_sub_addr, node_ip, port);
	subsock = bet_nanosock(0, bind_sub_addr, NN_SUB);

	bet_tcp_sock_address(0, bind_push_addr, node_ip, port + 1);
	pushsock = bet_nanosock(0, bind_push_addr, NN_PUSH);

	cashier_info = calloc(1, sizeof(struct cashier));

	cashier_info->c_subsock = subsock;
	cashier_info->c_pushsock = pushsock;
}

static void bet_cashier_deinitialize()
{
	if (cashier_info)
		free(cashier_info);
}

static void bet_player_initialize(char *dcv_ip, const int32_t port)
{
	int32_t subsock = -1, pushsock = -1;
	char bind_sub_addr[128], bind_push_addr[128];

	bet_tcp_sock_address(0, bind_sub_addr, dcv_ip, port);
	subsock = bet_nanosock(0, bind_sub_addr, NN_SUB);

	bet_tcp_sock_address(0, bind_push_addr, dcv_ip, port + 1);
	pushsock = bet_nanosock(0, bind_push_addr, NN_PUSH);

	player_vars = calloc(1, sizeof(*player_vars));

	bet_player = calloc(1, sizeof(struct privatebet_info));
	bet_player->subsock = subsock;
	bet_player->pushsock = pushsock;
	bet_player->maxplayers = (max_players < CARDS777_MAXPLAYERS) ? max_players : CARDS777_MAXPLAYERS;
	bet_player->maxchips = CARDS777_MAXCHIPS;
	bet_player->chipsize = CARDS777_CHIPSIZE;
	bet_player->numplayers = max_players;
	bet_info_set(bet_player, "demo", poker_deck_size, 0, max_players);
}

static void bet_player_deinitialize()
{
	if (bet_player)
		free(bet_player);
	if (player_vars)
		free(player_vars);
}

static void bet_player_thrd(char *dcv_ip, const int32_t port)
{
	pthread_t player_thrd, player_backend;

	bet_player_initialize(dcv_ip, port);
	if (OS_thread_create(&player_thrd, NULL, (void *)bet_player_backend_loop, (void *)bet_player) != 0) {
		printf("error in launching bet_player_backend_loop\n");
		exit(-1);
	}
	if (OS_thread_create(&player_backend, NULL, (void *)bet_player_frontend_loop, NULL) != 0) {
		printf("error launching bet_player_frontend_loop\n");
		exit(-1);
	}
	if (pthread_join(player_thrd, NULL)) {
		printf("\nError in joining the main thread for player_thrd");
	}

	if (pthread_join(player_backend, NULL)) {
		printf("\nError in joining the main thread for player_backend");
	}
	bet_player_deinitialize();
}

static void bet_bvv_initialize(char *dcv_ip, const int32_t port)
{
	int32_t subsock = -1, pushsock = -1;
	char bind_sub_addr[128], bind_push_addr[128];

	bet_tcp_sock_address(0, bind_sub_addr, dcv_ip, port);
	subsock = bet_nanosock(0, bind_sub_addr, NN_SUB);

	bet_tcp_sock_address(0, bind_push_addr, dcv_ip, port + 1);
	pushsock = bet_nanosock(0, bind_push_addr, NN_PUSH);

	bvv_vars = calloc(1, sizeof(*bvv_vars));
	bet_bvv = calloc(1, sizeof(struct privatebet_info));
	bet_bvv->subsock = subsock;
	bet_bvv->pushsock = pushsock;
	bet_bvv->maxplayers = (max_players < CARDS777_MAXPLAYERS) ? max_players : CARDS777_MAXPLAYERS;
	bet_bvv->maxchips = CARDS777_MAXCHIPS;
	bet_bvv->chipsize = CARDS777_CHIPSIZE;
	bet_bvv->numplayers = max_players;
	bet_bvv->myplayerid = -1;
	bet_info_set(bet_bvv, "demo", poker_deck_size, 0, max_players);
}

static void bet_bvv_deinitialize()
{
	if (bet_bvv)
		free(bet_bvv);
	if (bvv_vars)
		free(bvv_vars);
}

static void bet_bvv_thrd(char *dcv_ip, const int32_t port)
{
	pthread_t bvv_thrd, bvv_backend;

	bet_bvv_initialize(dcv_ip, port);
	if (OS_thread_create(&bvv_thrd, NULL, (void *)bet_bvv_backend_loop, (void *)bet_bvv) != 0) {
		printf("error launching bet_bvv_backend_loop\n");
		exit(-1);
	}
	if (OS_thread_create(&bvv_backend, NULL, (void *)bet_bvv_frontend_loop, NULL) != 0) {
		printf("error launching bet_bvv_frontend_loop\n");
		exit(-1);
	}
	if (pthread_join(bvv_backend, NULL)) {
		printf("\nError in joining the main thread for bvv_backend");
	}
	if (pthread_join(bvv_thrd, NULL)) {
		printf("\nError in joining the main thread for bvv_thrd");
	}
	bet_bvv_deinitialize();
}

static void bet_dcv_initialize(char *dcv_ip, const int32_t port)
{
	int32_t pubsock = -1, pullsock = -1;
	char bind_pub_addr[128], bind_pull_addr[128];

	bet_tcp_sock_address(0, bind_pub_addr, dcv_ip, port);
	pubsock = bet_nanosock(1, bind_pub_addr, NN_PUB);

	bet_tcp_sock_address(0, bind_pull_addr, dcv_ip, port + 1);
	pullsock = bet_nanosock(1, bind_pull_addr, NN_PULL);

	bet_dcv = calloc(1, sizeof(struct privatebet_info));
	dcv_vars = calloc(1, sizeof(struct privatebet_vars));

	bet_dcv->pubsock = pubsock;
	bet_dcv->pullsock = pullsock;
	bet_dcv->maxplayers = (max_players < CARDS777_MAXPLAYERS) ? max_players : CARDS777_MAXPLAYERS;
	bet_dcv->maxchips = CARDS777_MAXCHIPS;
	bet_dcv->chipsize = CARDS777_CHIPSIZE;
	bet_dcv->numplayers = 0;
	bet_dcv->myplayerid = -2;
	bet_dcv->cardid = -1;
	bet_dcv->turni = -1;
	bet_dcv->no_of_turns = 0;
	bet_info_set(bet_dcv, "demo", poker_deck_size, 0, max_players);
}

static void bet_dcv_deinitialize()
{
	if (bet_dcv)
		free(bet_dcv);
	if (dcv_vars)
		free(dcv_vars);
}

static void bet_dcv_thrd(char *dcv_ip, const int32_t port)
{
	pthread_t /*live_thrd,*/ dcv_backend, dcv_thrd;

	bet_dcv_initialize(dcv_ip, port);
	if (OS_thread_create(&dcv_backend, NULL, (void *)bet_dcv_backend_loop, (void *)bet_dcv) != 0) {
		printf("error launching bet_dcv_backend_loop\n");
		exit(-1);
	}
	if (OS_thread_create(&dcv_thrd, NULL, (void *)bet_dcv_frontend_loop, NULL) != 0) {
		printf("error launching bet_dcv_frontend_loop\n");
		exit(-1);
	}
#if 0
	if (OS_thread_create(&live_thrd, NULL, (void *)bet_dcv_live_loop, (void *)bet_dcv) != 0) {
		printf("error launching bet_dcv_live_loop]n");
		exit(-1);
	}
	if (pthread_join(live_thrd, NULL)) {
		printf("\nError in joining the main thread for live_thrd");
	}
#endif
	if (pthread_join(dcv_backend, NULL)) {
		printf("\nError in joining the main thread for dcv_backend");
	}
	if (pthread_join(dcv_thrd, NULL)) {
		printf("\nError in joining the main thread for dcv_thrd");
	}
	bet_dcv_deinitialize();
}

static void bet_cashier_server_initialize(char *node_ip, const int32_t port)
{
	int32_t pubsock = -1, pullsock = -1;
	char bind_pub_addr[128], bind_pull_addr[128];

	bet_tcp_sock_address(0, bind_pub_addr, node_ip, port);
	pubsock = bet_nanosock(1, bind_pub_addr, NN_PUB);

	bet_tcp_sock_address(0, bind_pull_addr, node_ip, port + 1);
	pullsock = bet_nanosock(1, bind_pull_addr, NN_PULL);

	cashier_info = calloc(1, sizeof(struct cashier));

	cashier_info->c_pubsock = pubsock;
	cashier_info->c_pullsock = pullsock;
}

static void bet_cashier_server_thrd(char *node_ip, const int32_t port)
{
	pthread_t server_thrd;

	bet_cashier_server_initialize(node_ip, port);
	if (OS_thread_create(&server_thrd, NULL, (void *)bet_cashier_server_loop, (void *)cashier_info) != 0) {
		printf("error launching bet_dcv_live_loop]n");
		exit(-1);
	}
	if (pthread_join(server_thrd, NULL)) {
		printf("\nError in joining the main thread for live_thrd");
	}
	bet_cashier_deinitialize();
}

static void bet_display_usage()
{
	printf("\nFor DCV: ./bet dcv <dcv_ip_address>");
	printf("\nFor BVV: ./bet bvv <dcv_ip_address>");
	printf("\nFor Player: ./bet player <dcv_ip_address>");
	printf("\nFor Cashier: ./bet cashier");
	printf("\nFor Withdraw: ./bet withdraw amount addr");
}

static void common_init()
{
	OS_init();
	libgfshare_init();
	check_ln_chips_sync();

	bet_check_notaries();
	bet_compute_m_of_n_msig_addr();
	bet_set_table_id();
	bet_sqlite3_init();
}
int main(int argc, char **argv)
{
	uint16_t port = 7797, cashier_pub_sub_port = 7901;
	char dcv_ip[20];
	bet_get_db_instance();
	//bet_parse_notary_file();
	if (argc == 3) {
		common_init();
		strncpy(dcv_ip, argv[2], sizeof(dcv_ip));
		if (strcmp(argv[1], "dcv") == 0) {
			bet_parse_config_file();
			bet_dcv_thrd(dcv_ip, port);
		} else if (strcmp(argv[1], "bvv") == 0) {
			bet_bvv_thrd(dcv_ip, port);
		} else if (strcmp(argv[1], "player") == 0) {
			bet_player_thrd(dcv_ip, port);
		}
	} else if ((argc == 2) && (strcmp(argv[1], "cashier") == 0)) {
		common_init();
		bet_cashier_server_thrd(bet_get_etho_ip(), cashier_pub_sub_port);
	} else if ((argc == 4) && (strcmp(argv[1], "withdraw") == 0)) {
		cJSON *tx = NULL;
		tx = chips_transfer_funds(atof(argv[2]), argv[3]);
		printf("tx details::%s\n", cJSON_Print(tx));
	} else if ((argc == 2) && ((strcmp(argv[1], "-h") == 0) || (strcmp(argv[1], "h") == 0) ||
				   (strcmp(argv[1], "--help") == 0) || (strcmp(argv[1], "help") == 0))) {
		bet_display_usage();
	} else {
		printf("\nInvalid Usage, use the flag -h or --help to get more usage details\n");
		bet_display_usage();
	}
	return 0;
}

bits256 curve25519_fieldelement(bits256 hash)
{
	hash.bytes[0] &= 0xf8, hash.bytes[31] &= 0x7f, hash.bytes[31] |= 0x40;
	return (hash);
}

bits256 card_rand256(int32_t privkeyflag, int8_t index)
{
	bits256 randval;
	OS_randombytes(randval.bytes, sizeof(randval));
	if (privkeyflag != 0)
		randval.bytes[0] &= 0xf8, randval.bytes[31] &= 0x7f, randval.bytes[31] |= 0x40;
	randval.bytes[30] = index;
	return (randval);
}

struct pair256 deckgen_common(struct pair256 *randcards, int32_t numcards)
{
	int32_t i;
	struct pair256 key, tmp;
	key.priv = curve25519_keypair(&key.prod);
	for (i = 0; i < numcards; i++) {
		tmp.priv = card_rand256(1, i);
		tmp.prod = curve25519(tmp.priv, curve25519_basepoint9());
		randcards[i] = tmp;
	}
	return (key);
}

void deckgen_common2(struct pair256 *randcards, int32_t numcards)
{
	for (int32_t i = 0; i < numcards; i++)
		randcards[i].priv = curve25519_keypair(&randcards[i].prod);
}

struct pair256 deckgen_player(bits256 *playerprivs, bits256 *playercards, int32_t *permis, int32_t numcards)
{
	int32_t i;
	struct pair256 key, randcards[256];
	key = deckgen_common(randcards, numcards);
	bet_permutation(permis, numcards);
	for (i = 0; i < numcards; i++) {
		playerprivs[i] = randcards[i].priv; // permis[i]
		playercards[i] = curve25519(playerprivs[i], key.prod);
	}
	return (key);
}
int32_t sg777_deckgen_vendor(int32_t playerid, bits256 *cardprods, bits256 *finalcards, int32_t numcards,
			     bits256 *playercards,
			     bits256 deckid) // given playercards[], returns cardprods[] and finalcards[]
{
	static struct pair256 randcards[256];
	static bits256 active_deckid, hash_temp[CARDS777_MAXCARDS];
	int32_t retval = 1;
	bits256 hash, xoverz, tmp[256];

	if (bits256_cmp(deckid, active_deckid) != 0)
		deckgen_common2(randcards, numcards);
	else {
		retval = -1;
		goto end;
	}

	for (int32_t i = 0; i < numcards; i++) {
		xoverz = xoverz_donna(curve25519(randcards[i].priv, playercards[i]));
		vcalc_sha256(0, hash.bytes, xoverz.bytes, sizeof(xoverz));
		hash_temp[i] = hash; // optimization
		tmp[i] = fmul_donna(curve25519_fieldelement(hash), randcards[i].priv);
	}

	for (int32_t i = 0; i < numcards; i++) {
		finalcards[i] = tmp[permis_d[i]];
		g_hash[playerid][i] = hash_temp[permis_d[i]]; // optimization
		cardprods[i] = randcards[i].prod; // same cardprods[] returned for each player
	}
end:
	return retval;
}

struct pair256 p2p_bvv_init(bits256 *keys, struct pair256 b_key, bits256 *blindings, bits256 *blindedcards,
			    bits256 *finalcards, int32_t numcards, int32_t numplayers, int32_t playerid, bits256 deckid)
{
	int32_t i, j, M;
	uint8_t space[8192];
	bits256 cardshares[CARDS777_MAXPLAYERS];
	struct enc_share temp;

	for (i = 0; i < numcards; i++) {
		blindings[i] = rand256(1);
		blindedcards[i] = fmul_donna(finalcards[permis_b[i]], blindings[i]);
	}

	M = (numplayers / 2) + 1;

	gfshare_calc_sharenrs(sharenrs, numplayers, deckid.bytes,
			      sizeof(deckid)); // same for all players for this round

	for (i = 0; i < numcards; i++) {
		gfshare_calc_shares(cardshares[0].bytes, blindings[i].bytes, sizeof(bits256), sizeof(bits256), M,
				    numplayers, sharenrs, space, sizeof(space));
		// create combined allshares
		for (j = 0; j < numplayers; j++) {
			// printf("%s --> ",bits256_str(hexstr,cardshares[j]));
			bet_cipher_create(b_key.priv, keys[j], temp.bytes, cardshares[j].bytes, sizeof(cardshares[j]));
			memcpy(g_shares[numplayers * numcards * playerid + i * numplayers + j].bytes, temp.bytes,
			       sizeof(temp));
		}
	}
	return b_key;
}

cJSON *bet_read_json_file(char *file_name)
{
	FILE *fp = NULL;
	cJSON *json_data = NULL;
	char *data = NULL, buf[256];
	unsigned long data_size = 1024, buf_size = 256, temp_size = 0;
	unsigned long new_size = data_size;

	data = calloc(data_size, sizeof(char));
	if (!data) {
		goto end;
	}

	fp = fopen(file_name, "r");
	if (fp == NULL) {
		printf("%s::%d::Failed to open %s\n", __FUNCTION__, __LINE__, file_name);
		goto end;
	} else {
		while (fgets(buf, buf_size, fp) != NULL) {
			temp_size = temp_size + strlen(buf);
			if (temp_size >= new_size) {
				char *temp = calloc(new_size, sizeof(char));
				strncpy(temp, data, strlen(data));
				free(data);
				new_size = new_size * 2;
				data = calloc(new_size, sizeof(char));
				strncpy(data, temp, strlen(temp));
				free(temp);
				printf("MEMORY DOUBLED:: IN READING :: %s :: NEEDING MORE MEMORY\n", file_name);
			}
			strcat(data, buf);
			memset(buf, 0x00, buf_size);
		}
		printf("%s::%d::%s\n", __FUNCTION__, __LINE__, data);
		json_data = cJSON_CreateObject();
		json_data = cJSON_Parse(data);
	}
end:
	if (data)
		free(data);
	return json_data;
}

void bet_parse_config_file()
{
	cJSON *config_info = NULL;

	config_info = bet_read_json_file(dealer_config_file);
	if (config_info) {
		max_players = jint(config_info, "max_players");
		table_stack_in_chips = jdouble(config_info, "table_stack_in_chips");
		chips_tx_fee = jdouble(config_info, "chips_tx_fee");
	}
}

void bet_parse_notary_file()
{
	cJSON *notaries_info = NULL;

	notaries_info = bet_read_json_file(notaries_file);
	if (notaries_info) {
		no_of_notaries = cJSON_GetArraySize(notaries_info);
		notary_node_ips = (char **)malloc(no_of_notaries * sizeof(char *));
		notary_node_pubkeys = (char **)malloc(no_of_notaries * sizeof(char *));

		for (int32_t i = 0; i < no_of_notaries; i++) {
			cJSON *node_info = cJSON_CreateObject();
			node_info = cJSON_GetArrayItem(notaries_info, i);

			notary_node_ips[i] = (char *)malloc(strlen(jstr(node_info, "ip")) + 1);
			memset(notary_node_ips[i], 0x00, strlen(jstr(node_info, "ip")) + 1);

			notary_node_pubkeys[i] = (char *)malloc(strlen(jstr(node_info, "pubkey")) + 1);
			memset(notary_node_pubkeys[i], 0x00, strlen(jstr(node_info, "pubkey")) + 1);

			strncpy(notary_node_ips[i], jstr(node_info, "ip"), strlen(jstr(node_info, "ip")));
			strncpy(notary_node_pubkeys[i], jstr(node_info, "pubkey"), strlen(jstr(node_info, "pubkey")));
		}
	}
}
