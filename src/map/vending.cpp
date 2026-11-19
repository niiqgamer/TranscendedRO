// Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

#include "vending.hpp"

#include <algorithm>
#include <cstdlib> // atoi, strtoul
#include <ctime>
#include <string>

#include <common/malloc.hpp> // aMalloc, aFree
#include <common/nullpo.hpp>
#include <common/showmsg.hpp> // ShowInfo
#include <common/strlib.hpp>
#include <common/timer.hpp>  // DIFF_TICK

#include "achievement.hpp"
#include "atcommand.hpp"
#include "battle.hpp"
#include "buyingstore.hpp"
#include "buyingstore.hpp" // struct s_autotrade_entry, struct s_autotrader
#include "chrif.hpp"
#include "clif.hpp"
#include "intif.hpp"
#include "itemdb.hpp"
#include "map.hpp"
#include "log.hpp"
#include "npc.hpp"
#include "path.hpp"
#include "pc.hpp"
#include "pc_groups.hpp"

static uint32 vending_nextid = 0; ///Vending_id counter
static DBMap *vending_db; ///DB holder the vender : charid -> map_session_data

//Autotrader
static DBMap *vending_autotrader_db; /// Holds autotrader info: char_id -> struct s_autotrader
static void vending_autotrader_remove(struct s_autotrader *at, bool remove);
static int32 vending_autotrader_free(DBKey key, DBData *data, va_list ap);

/*==========================================
 * Extended Vending system
 * A shop sells for Zeny (vend_loot == 0), Cash Points (vend_loot == item_cash)
 * or a currency item from db/item_vending_db.yml (vend_loot == item ID).
 *------------------------------------------*/

enum e_vending_currency_type : uint8 {
	VENDCURRENCY_ZENY = 0,
	VENDCURRENCY_CASH,
	VENDCURRENCY_ITEM,
};

static e_vending_currency_type vending_currency_type( t_itemid currency ){
	if( currency == 0 || ( battle_config.item_zeny > 0 && currency == static_cast<t_itemid>( battle_config.item_zeny ) ) )
		return VENDCURRENCY_ZENY;
	if( battle_config.item_cash > 0 && currency == static_cast<t_itemid>( battle_config.item_cash ) )
		return VENDCURRENCY_CASH;
	return VENDCURRENCY_ITEM;
}

/**
 * Currencies a player can pick when opening a shop, in display order.
 * Zeny and Cash are represented by their dummy items (item_zeny/item_cash).
 * Empty when extended vending is disabled (= plain Zeny vending).
 */
std::vector<t_itemid> vending_currency_list(){
	std::vector<t_itemid> list;

	if( !battle_config.extended_vending )
		return list;

	if( battle_config.item_zeny > 0 && item_db.exists( battle_config.item_zeny ) )
		list.push_back( battle_config.item_zeny );
	if( battle_config.item_cash > 0 && item_db.exists( battle_config.item_cash ) )
		list.push_back( battle_config.item_cash );

	for( const auto& it : itemdb_vending ){
		t_itemid nameid = it.first;

		if( !item_db.exists( nameid ) || vending_currency_type( nameid ) != VENDCURRENCY_ITEM )
			continue;
		list.push_back( nameid );
	}

	return list;
}

/**
 * Whether a shop may sell for this currency with the current configuration.
 * @param currency: 0 for Zeny, item_cash for Cash Points or a currency item ID
 */
bool vending_currency_allowed( t_itemid currency ){
	std::vector<t_itemid> list = vending_currency_list();

	if( list.empty() )
		return currency == 0; // Extended vending off or nothing configured: Zeny only

	t_itemid wanted = ( currency == 0 ) ? static_cast<t_itemid>( battle_config.item_zeny ) : currency;

	if( wanted == 0 )
		return false; // Zeny shops disabled (item_zeny: 0)

	return std::find( list.begin(), list.end(), wanted ) != list.end();
}

const char* vending_currency_name( t_itemid currency ){
	switch( vending_currency_type( currency ) ){
		case VENDCURRENCY_ZENY:
			return "Zeny";
		case VENDCURRENCY_CASH:
			return "Cash Points";
		default: {
			std::shared_ptr<item_data> id = item_db.find( currency );

			return id != nullptr ? id->ename.c_str() : "Unknown";
		}
	}
}

/// Formats a number with thousands separators (1234567 -> "1,234,567")
static std::string vending_format_number( int64 value ){
	std::string digits = std::to_string( value < 0 ? -value : value );
	std::string out;
	int32 n = 0;

	for( auto it = digits.rbegin(); it != digits.rend(); ++it, ++n ){
		if( n > 0 && n % 3 == 0 )
			out.insert( out.begin(), ',' );
		out.insert( out.begin(), *it );
	}

	if( value < 0 )
		out.insert( out.begin(), '-' );

	return out;
}

/// Whether an inventory item may be used to pay in a currency-item shop
static bool vending_currency_item_usable( const map_session_data& sd, const item& it, t_itemid currency, bool allow_bound ){
	if( it.nameid != currency || it.amount <= 0 )
		return false;
	if( it.equip || it.expire_time || !it.identify || it.attribute == 1 )
		return false;
	if( it.bound && !allow_bound )
		return false;
	if( !itemdb_cantrade( &it, pc_get_group_level( &sd ), pc_get_group_level( &sd ) ) )
		return false;
	return true;
}

/// Amount of currency items the buyer can pay with
static int64 vending_currency_item_count( const map_session_data& sd, t_itemid currency, bool allow_bound ){
	int64 total = 0;

	for( int32 i = 0; i < MAX_INVENTORY; i++ ){
		if( vending_currency_item_usable( sd, sd.inventory.u.items_inventory[i], currency, allow_bound ) )
			total += sd.inventory.u.items_inventory[i].amount;
	}

	return total;
}

/// Removes `amount` currency items from the buyer, across as many stacks as needed
static void vending_currency_item_delete( map_session_data& sd, t_itemid currency, int32 amount ){
	bool allow_bound = battle_config.ex_buying_bound != 0;

	for( int32 i = 0; i < MAX_INVENTORY && amount > 0; i++ ){
		const item& it = sd.inventory.u.items_inventory[i];

		if( !vending_currency_item_usable( sd, it, currency, allow_bound ) )
			continue;

		int32 take = std::min<int32>( amount, it.amount );

		pc_delitem( &sd, i, take, 0, 0, LOG_TYPE_VENDING );
		amount -= take;
	}
}

/// Sends the shop setup window (saving the cart first if needed). This is the stock MC_VENDING behavior.
static void vending_open_setup_ui( map_session_data& sd ){
	int32 i = 0;

	sd.state.prevend = 1;
	sd.state.workinprogress = WIP_DISABLE_ALL;
	ARR_FIND(0, MAX_CART, i, sd.cart.u.items_cart[i].nameid && sd.cart.u.items_cart[i].id == 0);
	if (i < MAX_CART) {
		// Save the cart before opening the vending UI
		sd.state.pending_vending_ui = true;
		intif_storage_save(&sd, &sd.cart);
	} else {
		// Instantly open the vending UI
		sd.state.pending_vending_ui = false;
		clif_openvendingreq(sd, 2 + sd.vend_skill_lv);
	}
}

/**
 * MC_VENDING was cast: show the currency list, or open the shop setup directly
 * when there is no choice to make.
 * @param sd: Player
 * @param skill_lv: Vending skill level used
 */
void vending_start_setup( map_session_data& sd, uint16 skill_lv ){
	std::vector<t_itemid> list = vending_currency_list();

	sd.vend_skill_lv = skill_lv;
	sd.vend_loot = 0;

	if( list.size() > 1 ){
		// Nothing is locked until a currency is picked, so closing the list leaves the player free
		clif_vending_currency_list( sd, list );
		return;
	}

	if( list.size() == 1 ){
		char output[CHAT_SIZE_MAX];

		sd.vend_loot = ( vending_currency_type( list[0] ) == VENDCURRENCY_ZENY ) ? 0 : list[0];
		safesnprintf( output, sizeof( output ), msg_txt( &sd, 1596 ), vending_currency_name( sd.vend_loot ) ); // Shop currency: %s
		clif_messagecolor( &sd, color_table[COLOR_CYAN], output, false, SELF );
	}

	vending_open_setup_ui( sd );
}

/**
 * Player picked a currency from the list sent by vending_start_setup.
 * @param sd: Player
 * @param nameid: Item ID the client sent back (0 = cancelled)
 */
void vending_select_currency( map_session_data& sd, t_itemid nameid ){
	if( nameid == 0 )
		return; // cancelled

	// The player could have changed state while the list was open
	if( sd.state.vending || sd.state.prevend || pc_isdead( &sd ) || pc_istrading( &sd ) || !pc_iscarton( &sd ) || !pc_checkskill( &sd, MC_VENDING ) || !pc_can_give_items( &sd ) ){
		clif_skill_fail( sd, MC_VENDING );
		return;
	}
	if( map_getmapflag( sd.m, MF_NOVENDING ) ){
		clif_displaymessage( sd.fd, msg_txt( &sd, 276 ) ); // "You can't open a shop on this map"
		clif_skill_fail( sd, MC_VENDING );
		return;
	}
	if( map_getcell( sd.m, sd.x, sd.y, CELL_CHKNOVENDING ) ){
		clif_displaymessage( sd.fd, msg_txt( &sd, 204 ) ); // "You can't open a shop on this cell."
		clif_skill_fail( sd, MC_VENDING );
		return;
	}
	if( npc_isnear( &sd ) ){
		clif_skill_fail( sd, MC_VENDING, USESKILL_FAIL_THERE_ARE_NPC_AROUND );
		return;
	}

	std::vector<t_itemid> list = vending_currency_list();

	if( std::find( list.begin(), list.end(), nameid ) == list.end() ){
		clif_displaymessage( sd.fd, msg_txt( &sd, 1605 ) ); // This currency cannot be used for vending.
		clif_skill_fail( sd, MC_VENDING );
		return;
	}

	char output[CHAT_SIZE_MAX];

	sd.vend_loot = ( vending_currency_type( nameid ) == VENDCURRENCY_ZENY ) ? 0 : nameid;
	safesnprintf( output, sizeof( output ), msg_txt( &sd, 1594 ), vending_currency_name( sd.vend_loot ) ); // Shop currency selected: %s
	clif_messagecolor( &sd, color_table[COLOR_CYAN], output, false, SELF );

	vending_open_setup_ui( sd );
}

/**
 * Lookup to get the vending_db outside module
 * @return the vending_db
 */
DBMap * vending_getdb()
{
	return vending_db;
}

/**
 * Create an unique vending shop id.
 * @return the next vending_id
 */
static int32 vending_getuid(void)
{
	return ++vending_nextid;
}

/**
 * Make a player close his shop
 * @param sd : player session
 */
void vending_closevending(map_session_data* sd)
{
	nullpo_retv(sd);

	if( sd->state.vending ) {
		if( Sql_Query( mmysql_handle, "DELETE FROM `%s` WHERE vending_id = %d;", vending_items_table, sd->vender_id ) != SQL_SUCCESS ||
			Sql_Query( mmysql_handle, "DELETE FROM `%s` WHERE `id` = %d;", vendings_table, sd->vender_id ) != SQL_SUCCESS ) {
				Sql_ShowDebug(mmysql_handle);
		}

		sd->state.vending = false;
		sd->vender_id = 0;
		sd->vend_loot = 0; // Extended Vending
		clif_closevendingboard( *sd, AREA_WOS, nullptr );
		idb_remove(vending_db, sd->status.char_id);
	}
}

/**
 * Player request a shop's item list (a player shop)
 * @param sd : player requestion the list
 * @param id : vender account id (gid)
 */
void vending_vendinglistreq(map_session_data* sd, int32 id)
{
	map_session_data* vsd;
	nullpo_retv(sd);

	if( (vsd = map_id2sd(id)) == nullptr )
		return;
	if( !vsd->state.vending )
		return; // not vending

	if (!pc_can_give_items(sd) || !pc_can_give_items(vsd)) { //check if both GMs are allowed to trade
		clif_displaymessage( sd->fd, msg_txt( sd, 246 ) ); // Your GM level doesn't authorize you to perform this action.
		return;
	}

	// Extended Vending: tell the buyer what this shop sells for
	if( vsd->vend_loot != 0 ){
		char output[CHAT_SIZE_MAX];

		safesnprintf( output, sizeof( output ), msg_txt( sd, 1595 ), vsd->status.name, vending_currency_name( vsd->vend_loot ) ); // %s's shop sells for: %s
		if( battle_config.show_broadcast_info )
			clif_broadcast( sd, output, strlen( output ) + 1, BC_BLUE, SELF );
		else
			clif_messagecolor( sd, color_table[COLOR_CYAN], output, false, SELF );
	}

	sd->vended_id = vsd->vender_id;  // register vending uid

	clif_vendinglist( *sd, *vsd );
}

/**
 * Calculates taxes for vending
 * @param sd: Vender
 * @param zeny: Total amount to tax
 * @return Total amount after taxes
 */
static double vending_calc_tax(map_session_data *sd, double zeny)
{
	if (battle_config.vending_tax && zeny >= battle_config.vending_tax_min)
		zeny -= zeny * (battle_config.vending_tax / 10000.);

	return zeny;
}

/**
 * Purchase item(s) from a shop
 * @param sd : buyer player session
 * @param aid : account id of vender
 * @param uid : shop unique id
 * @param data : items data who would like to purchase \n
 *	data := {<index>.w <amount>.w }[count]
 * @param count : number of different items he's trying to buy
 */
void vending_purchasereq(map_session_data* sd, int32 aid, int32 uid, const uint8* data, int32 count)
{
	int32 i, j, cursor, w, new_ = 0, blank, vend_list[MAX_VENDING];
	double z;
	struct s_vending vending[MAX_VENDING]; // against duplicate packets
	map_session_data* vsd = map_id2sd(aid);

	nullpo_retv(sd);
	if( vsd == nullptr || !vsd->state.vending || vsd->id == sd->id )
		return; // invalid shop

	if( vsd->vender_id != uid ) { // shop has changed
		clif_buyvending( *sd, 0, 0, PURCHASEMC_STORE_INCORRECT );  // store information was incorrect
		return;
	}

	if( !searchstore_queryremote(*sd, aid) && ( sd->m != vsd->m || !check_distance_bl(sd, vsd, AREA_SIZE) ) )
		return; // shop too far away

	searchstore_clearremote(*sd);

	if( count < 1 || count > MAX_VENDING || count > vsd->vend_num )
		return; // invalid amount of purchased items

	blank = pc_inventoryblank(sd); //number of free cells in the buyer's inventory

	// duplicate item in vending to check hacker with multiple packets
	memcpy(&vending, &vsd->vending, sizeof(vsd->vending)); // copy vending list

	// Extended Vending: what this shop sells for
	const t_itemid currency = vsd->vend_loot;
	const e_vending_currency_type currency_type = vending_currency_type( currency );
	char output[CHAT_SIZE_MAX];

	if( currency != 0 && !vending_currency_allowed( currency ) ){
		clif_displaymessage( sd->fd, msg_txt( sd, 1605 ) ); // This currency cannot be used for vending.
		return;
	}

	// some checks
	z = 0.; // zeny counter
	w = 0;  // weight counter
	for( i = 0; i < count; i++ ) {
		int16 amount = *(uint16*)(data + 4*i + 0);
		int16 idx    = *(uint16*)(data + 4*i + 2);
		idx -= 2;

		if( amount <= 0 )
			return;

		// check of item index in the cart
		if( idx < 0 || idx >= MAX_CART )
			return;

		ARR_FIND( 0, vsd->vend_num, j, vsd->vending[j].index == idx );
		if( j == vsd->vend_num )
			return; //picked non-existing item
		else
			vend_list[i] = j;

		z += ((double)vsd->vending[j].value * (double)amount);
		if( z < 0. || z > (double)MAX_ZENY ) {
			clif_buyvending( *sd, idx, amount, PURCHASEMC_NO_ZENY ); // total price out of range
			return;
		}

		switch( currency_type ){
			case VENDCURRENCY_ZENY:
				if( z > (double)sd->status.zeny ) {
					clif_buyvending( *sd, idx, amount, PURCHASEMC_NO_ZENY ); // you don't have enough zeny
					return;
				}
				if( z + (double)vsd->status.zeny > (double)MAX_ZENY ) {
					clif_buyvending( *sd, idx, vsd->vending[j].amount, PURCHASEMC_OUT_OF_STOCK ); // too much zeny = overflow
					return;
				}
				break;

			case VENDCURRENCY_CASH:
				if( z > (double)sd->cashPoints ) {
					clif_displaymessage( sd->fd, msg_txt( sd, 1590 ) ); // You do not have enough Cash Points.
					return;
				}
				if( z + (double)vsd->cashPoints > (double)MAX_CASHPOINT ) {
					clif_displaymessage( sd->fd, msg_txt( sd, 1603 ) ); // The seller cannot hold any more Cash Points.
					return;
				}
				break;

			case VENDCURRENCY_ITEM:
				if( z > (double)MAX_AMOUNT ) {
					safesnprintf( output, sizeof( output ), msg_txt( sd, 1593 ), vending_currency_name( currency ) ); // The seller cannot receive that many %s.
					clif_displaymessage( sd->fd, output );
					return;
				}
				if( z > (double)vending_currency_item_count( *sd, currency, battle_config.ex_buying_bound != 0 ) ) {
					if( !battle_config.ex_buying_bound && z <= (double)vending_currency_item_count( *sd, currency, true ) )
						safesnprintf( output, sizeof( output ), msg_txt( sd, 1604 ), vending_currency_name( currency ) ); // Bound %s cannot be used for payment.
					else
						safesnprintf( output, sizeof( output ), msg_txt( sd, 1591 ), vending_currency_name( currency ) ); // You do not have enough %s.
					clif_displaymessage( sd->fd, output );
					return;
				}
				break;
		}
		w += itemdb_weight(vsd->cart.u.items_cart[idx].nameid) * amount;
		if( w + sd->weight > sd->max_weight ) {
			clif_buyvending( *sd, idx, amount, PURCHASEMC_OVERWEIGHT );
			return;
		}

		//Check to see if cart/vend info is in sync.
		if( vending[j].amount > vsd->cart.u.items_cart[idx].amount )
			vending[j].amount = vsd->cart.u.items_cart[idx].amount;

		// if they try to add packets (example: get twice or more 2 apples if marchand has only 3 apples).
		// here, we check cumulative amounts
		if( vending[j].amount < amount ) {
			// send more quantity is not a hack (an other player can have buy items just before)
			clif_buyvending( *sd, idx, vsd->vending[j].amount, PURCHASEMC_OUT_OF_STOCK );
			return;
		}

		vending[j].amount -= amount;

		switch( pc_checkadditem(sd, vsd->cart.u.items_cart[idx].nameid, amount) ) {
		case CHKADDITEM_EXIST:
			break;	//We'd add this item to the existing one (in buyers inventory)
		case CHKADDITEM_NEW:
			new_++;
			if (new_ > blank)
				return; //Buyer has no space in his inventory
			break;
		case CHKADDITEM_OVERAMOUNT:
			return; //too many items
		}
	}

	const int32 total_price = (int32)z;

	switch( currency_type ){
		case VENDCURRENCY_ZENY:
			pc_payzeny(sd, (int32)z, LOG_TYPE_VENDING, vsd->status.char_id);
			achievement_update_objective(sd, AG_SPEND_ZENY, 1, (int32)z);
			z = vending_calc_tax(sd, z);
			pc_getzeny(vsd, (int32)z, LOG_TYPE_VENDING, sd->status.char_id);
			break;

		case VENDCURRENCY_CASH:
			if( pc_paycash( sd, total_price, 0, LOG_TYPE_VENDING ) < 0 ) {
				clif_displaymessage( sd->fd, msg_txt( sd, 1590 ) ); // You do not have enough Cash Points.
				return;
			}
			pc_getcash( vsd, total_price, 0, LOG_TYPE_VENDING );
			break;

		case VENDCURRENCY_ITEM: {
			// The seller must be able to receive the whole payment
			switch( pc_checkadditem( vsd, currency, total_price ) ) {
				case CHKADDITEM_OVERAMOUNT:
					safesnprintf( output, sizeof( output ), msg_txt( sd, 1593 ), vending_currency_name( currency ) ); // The seller cannot receive that many %s.
					clif_displaymessage( sd->fd, output );
					return;
				case CHKADDITEM_NEW:
					if( pc_inventoryblank( vsd ) <= 0 ) {
						safesnprintf( output, sizeof( output ), msg_txt( sd, 1592 ), vending_currency_name( currency ) ); // The seller has no free inventory space to receive %s.
						clif_displaymessage( sd->fd, output );
						return;
					}
					break;
			}
			if( (int64)itemdb_weight( currency ) * total_price + vsd->weight > vsd->max_weight ) {
				safesnprintf( output, sizeof( output ), msg_txt( sd, 1593 ), vending_currency_name( currency ) ); // The seller cannot receive that many %s.
				clif_displaymessage( sd->fd, output );
				return;
			}

			struct item payment = {};

			payment.nameid = currency;
			payment.identify = 1;

			// Give to the seller first: if it fails nothing has been taken from the buyer yet
			if( pc_additem( vsd, &payment, total_price, LOG_TYPE_VENDING ) != ADDITEM_SUCCESS ) {
				safesnprintf( output, sizeof( output ), msg_txt( sd, 1593 ), vending_currency_name( currency ) ); // The seller cannot receive that many %s.
				clif_displaymessage( sd->fd, output );
				return;
			}
			vending_currency_item_delete( *sd, currency, total_price );
			} break;
	}

	std::string report; // Extended Vending: RODEX sale report
	int32 report_lines = 0;
	int64 report_revenue = 0;

	for( i = 0; i < count; i++ ) {
		int16 amount = *(uint16*)(data + 4*i + 0);
		int16 idx    = *(uint16*)(data + 4*i + 2);
		idx -= 2;
		z = 0.; // zeny counter

		// vending item
		pc_additem(sd, &vsd->cart.u.items_cart[idx], amount, LOG_TYPE_VENDING);
		vsd->vending[vend_list[i]].amount -= amount;
		z += ((double)vsd->vending[vend_list[i]].value * (double)amount);

		if( vsd->vending[vend_list[i]].amount ) {
			if( Sql_Query( mmysql_handle, "UPDATE `%s` SET `amount` = %d WHERE `vending_id` = %d and `cartinventory_id` = %d", vending_items_table, vsd->vending[vend_list[i]].amount, vsd->vender_id, vsd->cart.u.items_cart[idx].id ) != SQL_SUCCESS ) {
				Sql_ShowDebug( mmysql_handle );
			}
		} else {
			if( Sql_Query( mmysql_handle, "DELETE FROM `%s` WHERE `vending_id` = %d and `cartinventory_id` = %d", vending_items_table, vsd->vender_id, vsd->cart.u.items_cart[idx].id ) != SQL_SUCCESS ) {
				Sql_ShowDebug( mmysql_handle );
			}
		}

		// Save the sold item's ID before it may be removed from the cart
		t_itemid sold_nameid = vsd->cart.u.items_cart[idx].nameid;

		pc_cart_delitem(vsd, idx, amount, 0, LOG_TYPE_VENDING);

		// Tax only applies to Zeny
		if( currency_type == VENDCURRENCY_ZENY )
			z = vending_calc_tax(sd, z);
		clif_vendingreport( *vsd, idx, amount, sd->status.char_id, currency_type == VENDCURRENCY_ZENY ? (int32)z : 0 );

		//print buyer's name
		if( battle_config.buyer_name ) {
			char temp[256];

			if( battle_config.ex_vending_info ) // %s bought '%s' x %d. Revenue: %s %s
				safesnprintf( temp, sizeof( temp ), msg_txt( vsd, 1597 ), sd->status.name, itemdb_ename( sold_nameid ), amount, vending_format_number( (int64)z ).c_str(), vending_currency_name( currency ) );
			else
				safesnprintf( temp, sizeof( temp ), msg_txt( vsd, 265 ), sd->status.name );
			clif_messagecolor(vsd, color_table[COLOR_LIGHT_GREEN], temp, false, SELF);
		}

		// Extended Vending: collect the RODEX report
		if( report_lines < 5 ) {
			char line[128];

			safesnprintf( line, sizeof( line ), msg_txt( vsd, 1600 ), itemdb_ename( sold_nameid ), amount ); //  - %s x %d
			report += line;
			report += "\r\n";
		} else if( report_lines == 5 ) {
			report += msg_txt( vsd, 1601 ); //  ...and more
			report += "\r\n";
		}
		report_lines++;
		report_revenue += (int64)z;
	}

	// Extended Vending: total revenue of this purchase
	if( battle_config.ex_vending_info && count > 1 ) {
		safesnprintf( output, sizeof( output ), msg_txt( vsd, 1598 ), sd->status.name, vending_format_number( report_revenue ).c_str(), vending_currency_name( currency ) ); // Total revenue from %s: %s %s
		clif_messagecolor( vsd, color_table[COLOR_LIGHT_GREEN], output, false, SELF );
	}

	// Extended Vending: RODEX sale report to the seller
	if( battle_config.ex_vending_report && pc_readglobalreg( vsd, add_str( "NOVREPORT" ) ) == 0 ) {
		struct mail_message msg = {};
		char header[128], footer[128];

		safesnprintf( header, sizeof( header ), msg_txt( vsd, 1599 ), sd->status.name ); // [%s] bought from your shop:
		safesnprintf( footer, sizeof( footer ), msg_txt( vsd, 1602 ), vending_format_number( report_revenue ).c_str(), vending_currency_name( currency ) ); // Total revenue: %s %s

		std::string body = std::string( header ) + "\r\n" + report + "\r\n" + footer;

		msg.dest_id = vsd->status.char_id;
		safestrncpy( msg.dest_name, vsd->status.name, NAME_LENGTH );
		safestrncpy( msg.send_name, msg_txt( vsd, 1606 ), NAME_LENGTH ); // Vending
		safestrncpy( msg.title, msg_txt( vsd, 1607 ), MAIL_TITLE_LENGTH ); // Vending Report
		safestrncpy( msg.body, body.c_str(), MAIL_BODY_LENGTH );
		msg.status = MAIL_NEW;
		msg.type = MAIL_INBOX_NORMAL;
		msg.timestamp = time( nullptr );
		intif_Mail_send( 0, &msg );
	}

	// compact the vending list
	for( i = 0, cursor = 0; i < vsd->vend_num; i++ ) {
		if( vsd->vending[i].amount == 0 )
			continue;

		if( cursor != i ) { // speedup
			vsd->vending[cursor].index = vsd->vending[i].index;
			vsd->vending[cursor].amount = vsd->vending[i].amount;
			vsd->vending[cursor].value = vsd->vending[i].value;
		}

		cursor++;
	}

	vsd->vend_num = cursor;

	//Always save BOTH: customer (buyer) and vender
	if( save_settings&CHARSAVE_VENDING ) {
		chrif_save(sd, CSAVE_INVENTORY|CSAVE_CART);
		chrif_save(vsd, CSAVE_INVENTORY|CSAVE_CART);
	}

	//check for @AUTOTRADE users [durf]
	if( vsd->state.autotrade ) {
		//see if there is anything left in the shop
		ARR_FIND( 0, vsd->vend_num, i, vsd->vending[i].amount > 0 );
		if( i == vsd->vend_num ) {
			//Close Vending (this was automatically done by the client, we have to do it manually for autovenders) [Skotlex]
			vending_closevending(vsd);
			map_quit(vsd);	//They have no reason to stay around anymore, do they?
		}
	}
}

/**
 * Player setup a new shop
 * @param sd : player opening the shop
 * @param message : shop title
 * @param data : itemlist data
 *	data := {<index>.w <amount>.w <value>.l}[count]
 * @param count : number of different items
 * @param at Autotrader info, or nullptr if requetsed not from autotrade persistance
 * @return 0 If success, 1 - Cannot open (die, not state.prevend, trading), 2 - No cart, 3 - Count issue, 4 - Cart data isn't saved yet, 5 - No valid item found
 */
int8 vending_openvending( map_session_data& sd, const char* message, const uint8* data, int32 count, struct s_autotrader *at ){
	int32 i, j;
	int32 vending_skill_lvl;
	char message_sql[MESSAGE_SIZE*2];
	StringBuf buf;

	if ( pc_isdead(&sd) || !sd.state.prevend || pc_istrading(&sd)) {
		return 1; // can't open vendings lying dead || didn't use via the skill (wpe/hack) || can't have 2 shops at once
	}

	vending_skill_lvl = pc_checkskill(&sd, MC_VENDING);
	
	// skill level and cart check
	if( !vending_skill_lvl || !pc_iscarton(&sd) ) {
		clif_skill_fail( sd, MC_VENDING );
		sd.state.prevend = 0;
		sd.state.workinprogress = WIP_DISABLE_NONE;
		clif_openvending_ack( sd, OPENSTORE2_FAILED );
		return 2;
	}

	// check number of items in shop
	if( count < 1 || count > MAX_VENDING || count > 2 + vending_skill_lvl ) { // invalid item count
		clif_skill_fail( sd, MC_VENDING );
		sd.state.prevend = 0;
		sd.state.workinprogress = WIP_DISABLE_NONE;
		clif_openvending_ack( sd, OPENSTORE2_FAILED );
		return 3;
	}

	// Extended Vending: the shop currency must still be valid
	if( !vending_currency_allowed( sd.vend_loot ) ) {
		clif_displaymessage( sd.fd, msg_txt( &sd, 1605 ) ); // This currency cannot be used for vending.
		clif_skill_fail( sd, MC_VENDING );
		sd.state.prevend = 0;
		sd.state.workinprogress = WIP_DISABLE_NONE;
		clif_openvending_ack( sd, OPENSTORE2_FAILED );
		return 6;
	}

	if (save_settings&CHARSAVE_VENDING) // Avoid invalid data from saving
		chrif_save(&sd, CSAVE_INVENTORY|CSAVE_CART);

	// filter out invalid items
	i = 0;
	int64 total = 0;
	for( j = 0; j < count; j++ ) {
		int16 index        = *(uint16*)(data + 8*j + 0);
		int16 amount       = *(uint16*)(data + 8*j + 2);
		uint32 value       = *(uint32*)(data + 8*j + 4);

		index -= 2; // offset adjustment (client says that the first cart position is 2)

		if( index < 0 || index >= MAX_CART // invalid position
		||  pc_cartitem_amount(&sd, index, amount) < 0 // invalid item or insufficient quantity
		//NOTE: official server does not do any of the following checks!
		||  !sd.cart.u.items_cart[index].identify // unidentified item
		||  sd.cart.u.items_cart[index].attribute == 1 // broken item
		||  sd.cart.u.items_cart[index].expire_time // It should not be in the cart but just in case
		||  (sd.cart.u.items_cart[index].bound && !pc_can_give_bounded_items(&sd)) // can't trade account bound items and has no permission
		||  (sd.vend_loot != 0 && sd.cart.u.items_cart[index].nameid == sd.vend_loot) // Extended Vending: can't sell the shop currency itself
		||  !itemdb_cantrade(&sd.cart.u.items_cart[index], pc_get_group_level(&sd), pc_get_group_level(&sd)) ) // untradeable item
			continue;

		sd.vending[i].index = index;
		sd.vending[i].amount = amount;
		sd.vending[i].value = min(value, (uint32)battle_config.vending_max_value);
		total += static_cast<int64>(sd.vending[i].value) * amount;
		i++; // item successfully added
	}

	// check if the total value of the items plus the current zeny is over the limit
	// (Extended Vending: only relevant for Zeny shops)
	if ( vending_currency_type( sd.vend_loot ) == VENDCURRENCY_ZENY && !battle_config.vending_over_max && (static_cast<int64>(sd.status.zeny) + total) > MAX_ZENY ) {
#if PACKETVER >= 20200819
		clif_msg_color( sd, MSI_MERCHANTSHOP_TOTA_LOVER_ZENY_ERR, color_table[COLOR_RED] );
#endif
		clif_skill_fail( sd, MC_VENDING );
		sd.state.prevend = 0;
		sd.state.workinprogress = WIP_DISABLE_NONE;
		clif_openvending_ack( sd, OPENSTORE2_FAILED );
		return 1;
	}

	if (i != j) {
		clif_displaymessage(sd.fd, msg_txt(&sd, 266)); //"Some of your items cannot be vended and were removed from the shop."
		clif_skill_fail( sd, MC_VENDING ); // custom reply packet
		sd.state.prevend = 0;
		sd.state.workinprogress = WIP_DISABLE_NONE;
		clif_openvending_ack( sd, OPENSTORE2_FAILED );
		return 5;
	}

	if( i == 0 ) { // no valid item found
		clif_skill_fail( sd, MC_VENDING ); // custom reply packet
		sd.state.prevend = 0;
		sd.state.workinprogress = WIP_DISABLE_NONE;
		clif_openvending_ack( sd, OPENSTORE2_FAILED );
		return 5;
	}

	sd.state.prevend = 0;
	sd.state.vending = true;
	sd.state.workinprogress = WIP_DISABLE_NONE;
	sd.vender_id = vending_getuid();
	sd.vend_num = i;
	safestrncpy(sd.message, message, MESSAGE_SIZE);
	
	Sql_EscapeString( mmysql_handle, message_sql, sd.message );

	if( Sql_Query( mmysql_handle, "INSERT INTO `%s`(`id`, `account_id`, `char_id`, `sex`, `map`, `x`, `y`, `title`, `autotrade`, `body_direction`, `head_direction`, `sit`, `extended_vending_item`) "
		"VALUES( %d, %d, %d, '%c', '%s', %d, %d, '%s', %d, '%d', '%d', '%d', %u );",
		vendings_table, sd.vender_id, sd.status.account_id, sd.status.char_id, sd.status.sex == SEX_FEMALE ? 'F' : 'M', map_getmapdata(sd.m)->name, sd.x, sd.y, message_sql, sd.state.autotrade, at ? at->dir : sd.ud.dir, at ? at->head_dir : sd.head_dir, at ? at->sit : pc_issit(&sd), sd.vend_loot ) != SQL_SUCCESS ) {
		Sql_ShowDebug(mmysql_handle);
	}

	StringBuf_Init(&buf);
	StringBuf_Printf(&buf, "INSERT INTO `%s`(`vending_id`,`index`,`cartinventory_id`,`amount`,`price`) VALUES", vending_items_table);
	for (j = 0; j < i; j++) {
		StringBuf_Printf(&buf, "(%d,%d,%d,%d,%d)", sd.vender_id, j, sd.cart.u.items_cart[sd.vending[j].index].id, sd.vending[j].amount, sd.vending[j].value);
		if (j < i-1)
			StringBuf_AppendStr(&buf, ",");
	}
	if (SQL_ERROR == Sql_QueryStr(mmysql_handle, StringBuf_Value(&buf)))
		Sql_ShowDebug(mmysql_handle);

	clif_openvending( sd );
	clif_showvendingboard( sd );

	idb_put(vending_db, sd.status.char_id, &sd);

	return 0;
}

/**
 * Checks if an item is being sold in given player's vending.
 * @param sd : vender session (player)
 * @param nameid : item id
 * @return 0:not selling it, 1: yes
 */
bool vending_search( const map_session_data* sd, t_itemid nameid )
{
	int32 i;

	if( !sd->state.vending ) { // not vending
		return false;
	}

	ARR_FIND( 0, sd->vend_num, i, sd->cart.u.items_cart[sd->vending[i].index].nameid == nameid );
	if( i == sd->vend_num ) { // not found
		return false;
	}

	return true;
}

/**
 * Searches for all items in a vending, that match given ids, price and possible cards.
 * @param sd : The vender session to search into
 * @param s : parameter of the search (see s_search_store_search)
 * @return Whether or not the search should be continued.
 */
bool vending_searchall( const map_session_data* sd, const struct s_search_store_search* s )
{
	int32 i, c, slot;
	uint32 idx, cidx;
	const item* it;

	if( !sd->state.vending ) // not vending
		return true;

	for( idx = 0; idx < s->item_count; idx++ ) {
		ARR_FIND( 0, sd->vend_num, i, sd->cart.u.items_cart[sd->vending[i].index].nameid == s->itemlist[idx].itemId );
		if( i == sd->vend_num ) { // not found
			continue;
		}
		it = &sd->cart.u.items_cart[sd->vending[i].index];

		if( s->min_price && s->min_price > sd->vending[i].value ) { // too low price
			continue;
		}

		if( s->max_price && s->max_price < sd->vending[i].value ) { // too high price
			continue;
		}

		if( s->card_count ) { // check cards
			if( itemdb_isspecial(it->card[0]) ) { // something, that is not a carded
				continue;
			}
			slot = itemdb_slots(it->nameid);

			for( c = 0; c < slot && it->card[c]; c ++ ) {
				ARR_FIND( 0, s->card_count, cidx, s->cardlist[cidx].itemId == it->card[c] );
				if( cidx != s->card_count ) { // found
					break;
				}
			}

			if( c == slot || !it->card[c] ) { // no card match
				continue;
			}
		}

		// Check if the result set is full
		if( s->search_sd->searchstore.items.size() >= (uint32)battle_config.searchstore_maxresults ){
			return false;
		}

		std::shared_ptr<s_search_store_info_item> ssitem = std::make_shared<s_search_store_info_item>();

		ssitem->store_id = sd->vender_id;
		ssitem->account_id = sd->status.account_id;
		safestrncpy( ssitem->store_name, sd->message, sizeof( ssitem->store_name ) );
		ssitem->nameid = it->nameid;
		ssitem->amount = sd->vending[i].amount;
		ssitem->price = sd->vending[i].value;
		for( int32 j = 0; j < MAX_SLOTS; j++ ){
			ssitem->card[j] = it->card[j];
		}
		ssitem->refine = it->refine;
		ssitem->enchantgrade = it->enchantgrade;

		s->search_sd->searchstore.items.push_back( ssitem );
	}

	return true;
}

/**
* Open vending for Autotrader
* @param sd Player as autotrader
*/
void vending_reopen( map_session_data& sd )
{
	struct s_autotrader *at = nullptr;
	int8 fail = -1;

	// Open vending for this autotrader
	if ((at = (struct s_autotrader *)uidb_get(vending_autotrader_db, sd.status.char_id)) && at->count && at->entries) {
		uint8 *data, *p;
		uint16 j, count;

		// Init vending data for autotrader
		CREATE(data, uint8, at->count * 8);

		for (j = 0, p = data, count = at->count; j < at->count; j++) {
			struct s_autotrade_entry *entry = at->entries[j];
			uint16 *index = (uint16*)(p + 0);
			uint16 *amount = (uint16*)(p + 2);
			uint32 *value = (uint32*)(p + 4);

			// Find item position in cart
			ARR_FIND(0, MAX_CART, entry->index, sd.cart.u.items_cart[entry->index].id == entry->cartinventory_id);

			if (entry->index == MAX_CART) {
				count--;
				continue;
			}

			*index = entry->index + 2;
			*amount = itemdb_isstackable(sd.cart.u.items_cart[entry->index].nameid) ? entry->amount : 1;
			*value = entry->price;

			p += 8;
		}

		sd.state.prevend = 1; // Set him into a hacked prevend state
		sd.state.autotrade = 1;
		sd.vend_loot = at->vend_loot; // Extended Vending: restore the shop currency

		// Make sure abort all NPCs
		npc_event_dequeue(&sd);
		pc_cleareventtimer(&sd);

		// Open the vending again
		if( (fail = vending_openvending(sd, at->title, data, count, at)) == 0 ) {
			// Make vendor look perfect
			pc_setdir(&sd, at->dir, at->head_dir);
			clif_changed_dir(sd, AREA_WOS);
			if( at->sit ) {
				pc_setsit(&sd);
				skill_sit(&sd, 1);
				clif_sitting(sd);
			}

			// Immediate save
			chrif_save(&sd, CSAVE_AUTOTRADE);

			ShowInfo("Vending loaded for '" CL_WHITE "%s" CL_RESET "' with '" CL_WHITE "%d" CL_RESET "' items at " CL_WHITE "%s (%d,%d)" CL_RESET "\n",
				sd.status.name, count, mapindex_id2name(sd.mapindex), sd.x, sd.y);
		}
		aFree(data);
	}

	if (at) {
		vending_autotrader_remove(at, true);
		if (db_size(vending_autotrader_db) == 0)
			vending_autotrader_db->clear(vending_autotrader_db, vending_autotrader_free);
	}

	if (fail != 0) {
		ShowError("vending_reopen: (Error:%d) Load failed for autotrader '" CL_WHITE "%s" CL_RESET "' (CID=%d/AID=%d)\n", fail, sd.status.name, sd.status.char_id, sd.status.account_id);
		map_quit(&sd);
	}
}

/**
* Initializing autotraders from table
*/
void do_init_vending_autotrade(void)
{
	if (battle_config.feature_autotrade) {
		if (Sql_Query(mmysql_handle,
			"SELECT `id`, `account_id`, `char_id`, `sex`, `title`, `body_direction`, `head_direction`, `sit`, `extended_vending_item` "
			"FROM `%s` "
			"WHERE `autotrade` = 1 AND (SELECT COUNT(`vending_id`) FROM `%s` WHERE `vending_id` = `id`) > 0 "
			"ORDER BY `id`;",
			vendings_table, vending_items_table ) != SQL_SUCCESS )
		{
			Sql_ShowDebug(mmysql_handle);
			return;
		}

		if( Sql_NumRows(mmysql_handle) > 0 ) {
			uint16 items = 0;
			DBIterator *iter = nullptr;
			struct s_autotrader *at = nullptr;

			// Init each autotrader data
			while (SQL_SUCCESS == Sql_NextRow(mmysql_handle)) {
				size_t len;
				char* data;

				at = nullptr;
				CREATE(at, struct s_autotrader, 1);
				Sql_GetData(mmysql_handle, 0, &data, nullptr); at->id = atoi(data);
				Sql_GetData(mmysql_handle, 1, &data, nullptr); at->account_id = atoi(data);
				Sql_GetData(mmysql_handle, 2, &data, nullptr); at->char_id = atoi(data);
				Sql_GetData(mmysql_handle, 3, &data, nullptr); at->sex = (data[0] == 'F') ? SEX_FEMALE : SEX_MALE;
				Sql_GetData(mmysql_handle, 4, &data, &len); safestrncpy(at->title, data, zmin(len + 1, MESSAGE_SIZE));
				Sql_GetData(mmysql_handle, 5, &data, nullptr); at->dir = atoi(data);
				Sql_GetData(mmysql_handle, 6, &data, nullptr); at->head_dir = atoi(data);
				Sql_GetData(mmysql_handle, 7, &data, nullptr); at->sit = atoi(data);
				Sql_GetData(mmysql_handle, 8, &data, nullptr); at->vend_loot = static_cast<t_itemid>( strtoul( data, nullptr, 10 ) );
				at->count = 0;

				// Extended Vending: never reopen a Cash/item shop as a Zeny shop (prices would be wrong).
				// If its currency is no longer allowed, skip it; the items stay in the cart.
				if( !vending_currency_allowed( at->vend_loot ) ){
					ShowWarning( "do_init_vending_autotrade: Skipping autotrader (CID=%d/AID=%d), its shop currency %u is not allowed anymore.\n", at->char_id, at->account_id, at->vend_loot );
					aFree( at );
					continue;
				}

				if (battle_config.feature_autotrade_direction >= 0)
					at->dir = battle_config.feature_autotrade_direction;
				if (battle_config.feature_autotrade_head_direction >= 0)
					at->head_dir = battle_config.feature_autotrade_head_direction;
				if (battle_config.feature_autotrade_sit >= 0)
					at->sit = battle_config.feature_autotrade_sit;

				// initialize player
				CREATE(at->sd, map_session_data, 1); // TODO: Dont use Memory Manager allocation anymore and rely on the C++ container
				new (at->sd) map_session_data();
				pc_setnewpc(at->sd, at->account_id, at->char_id, 0, gettick(), at->sex, 0);
				at->sd->state.autotrade = 1|2;
				if (battle_config.autotrade_monsterignore)
					at->sd->state.block_action |= PCBLOCK_IMMUNE;
				else
					at->sd->state.block_action &= ~PCBLOCK_IMMUNE;
				chrif_authreq(at->sd, true);
				uidb_put(vending_autotrader_db, at->char_id, at);
			}
			Sql_FreeResult(mmysql_handle);

			// Init items for each autotraders
			iter = db_iterator(vending_autotrader_db);
			for (at = (struct s_autotrader *)dbi_first(iter); dbi_exists(iter); at = (struct s_autotrader *)dbi_next(iter)) {
				uint16 j = 0;

				if (SQL_ERROR == Sql_Query(mmysql_handle,
					"SELECT `cartinventory_id`, `amount`, `price` "
					"FROM `%s` "
					"WHERE `vending_id` = %d "
					"ORDER BY `index` ASC;",
					vending_items_table, at->id ) )
				{
					Sql_ShowDebug(mmysql_handle);
					continue;
				}

				if (!(at->count = (uint16)Sql_NumRows(mmysql_handle))) {
					map_quit(at->sd);
					vending_autotrader_remove(at, true);
					continue;
				}

				//Init the list
				CREATE(at->entries, struct s_autotrade_entry *, at->count);

				//Add the item into list
				j = 0;
				while (SQL_SUCCESS == Sql_NextRow(mmysql_handle) && j < at->count) {
					char *data;
					CREATE(at->entries[j], struct s_autotrade_entry, 1);
					Sql_GetData(mmysql_handle, 0, &data, nullptr); at->entries[j]->cartinventory_id = atoi(data);
					Sql_GetData(mmysql_handle, 1, &data, nullptr); at->entries[j]->amount = atoi(data);
					Sql_GetData(mmysql_handle, 2, &data, nullptr); at->entries[j]->price = atoi(data);
					j++;
				}
				items += j;
				Sql_FreeResult(mmysql_handle);
			}
			dbi_destroy(iter);

			ShowStatus("Done loading '" CL_WHITE "%d" CL_RESET "' vending autotraders with '" CL_WHITE "%d" CL_RESET "' items.\n", db_size(vending_autotrader_db), items);
		}
	}

	// Everything is loaded fine, their entries will be reinserted once they are loaded
	if (Sql_Query( mmysql_handle, "DELETE FROM `%s`;", vendings_table ) != SQL_SUCCESS ||
		Sql_Query( mmysql_handle, "DELETE FROM `%s`;", vending_items_table ) != SQL_SUCCESS) {
		Sql_ShowDebug(mmysql_handle);
	}
}

/**
 * Remove an autotrader's data
 * @param at Autotrader
 * @param remove If true will removes from vending_autotrader_db
 **/
static void vending_autotrader_remove(struct s_autotrader *at, bool remove) {
	nullpo_retv(at);
	if (at->count && at->entries) {
		uint16 i = 0;
		for (i = 0; i < at->count; i++) {
			if (at->entries[i])
				aFree(at->entries[i]);
		}
		aFree(at->entries);
	}
	if (remove)
		uidb_remove(vending_autotrader_db, at->char_id);
	aFree(at);
}

/**
* Clear all autotraders
* @author [Cydh]
*/
static int32 vending_autotrader_free(DBKey key, DBData *data, va_list ap) {
	struct s_autotrader *at = (struct s_autotrader *)db_data2ptr(data);
	if (at)
		vending_autotrader_remove(at, false);
	return 0;
}

/**
* Update vendor location
* @param sd: Player's session data
*/
void vending_update(map_session_data &sd)
{
	if (Sql_Query(mmysql_handle, "UPDATE `%s` SET `map` = '%s', `x` = '%d', `y` = '%d', `body_direction` = '%d', `head_direction` = '%d', `sit` = '%d', `autotrade` = '%d' WHERE `id` = '%d'",
		vendings_table, map_getmapdata(sd.m)->name, sd.x, sd.y, sd.ud.dir, sd.head_dir, pc_issit(&sd), sd.state.autotrade,
		sd.vender_id
	) != SQL_SUCCESS) {
		Sql_ShowDebug(mmysql_handle);
	}
}

/**	
 * Initialise the vending module
 * called in map::do_init
 */
void do_final_vending(void)
{
	db_destroy(vending_db);
	vending_autotrader_db->destroy(vending_autotrader_db, vending_autotrader_free);
}

/**
 * Destory the vending module
 * called in map::do_final
 */
void do_init_vending(void)
{
	vending_db = idb_alloc(DB_OPT_BASE);
	vending_autotrader_db = uidb_alloc(DB_OPT_BASE);
	vending_nextid = 0;
}
