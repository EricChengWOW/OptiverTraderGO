// Copyright 2021 Optiver Asia Pacific Pty. Ltd.
//
// This file is part of Ready Trader Go.
//
//     Ready Trader Go is free software: you can redistribute it and/or
//     modify it under the terms of the GNU Affero General Public License
//     as published by the Free Software Foundation, either version 3 of
//     the License, or (at your option) any later version.
//
//     Ready Trader Go is distributed in the hope that it will be useful,
//     but WITHOUT ANY WARRANTY; without even the implied warranty of
//     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//     GNU Affero General Public License for more details.
//
//     You should have received a copy of the GNU Affero General Public
//     License along with Ready Trader Go.  If not, see
//     <https://www.gnu.org/licenses/>.
#include <array>

#include <boost/asio/io_context.hpp>

#include <ready_trader_go/logging.h>

#include "autotrader.h"

using namespace std::chrono;

using namespace ReadyTraderGo;

RTG_INLINE_GLOBAL_LOGGER_WITH_CHANNEL(LG_AT, "AUTO")

constexpr int LOT_SIZE = 8;
constexpr int POSITION_LIMIT = 100;
constexpr int TICK_SIZE_IN_CENTS = 100;
constexpr int MIN_BID_NEARST_TICK = (MINIMUM_BID + TICK_SIZE_IN_CENTS) / TICK_SIZE_IN_CENTS * TICK_SIZE_IN_CENTS;
constexpr int MAX_ASK_NEAREST_TICK = MAXIMUM_ASK / TICK_SIZE_IN_CENTS * TICK_SIZE_IN_CENTS;

AutoTrader::AutoTrader(boost::asio::io_context& context) : BaseAutoTrader(context)
{
}

void AutoTrader::insert_event() {
    /* Insert an message time to the log
        Delete all message log 1 second before
    */
    auto cur = steady_clock::now();
    long time = duration_cast<milliseconds>(cur - base_time).count();
    recent_activity.push_back(time);
    
    unsigned long index = 0;
    for (int i = 0 ; i < recent_activity.size(); i++) {
        if (time - recent_activity[i] > bound_time) {
            index += 1;
        } else {
            break;
        }
    }
    
    while (index > 0) {
        recent_activity.erase(recent_activity.begin());
        index --;
    }
}

void AutoTrader::wait_event_space() {
    /* Wait until there is an available message slot in the 50/sec limit
        If not existing currently, wait until the earliest to be deleted
    */
    if (recent_activity.size() < message_limit) {
        return;
    }
    
    RLOG(LG_AT, LogLevel::LL_INFO) << "Waiting for event space";

    while (true) {
        auto cur = steady_clock::now();
        long time = duration_cast<milliseconds>(cur - base_time).count();
        unsigned long index = 0;
        if (time - recent_activity[0] > bound_time) {
            for (int i = 0 ; i < recent_activity.size(); i++) {
                if (time - recent_activity[i] > bound_time) {
                    index += 1;
                } else {
                    break;
                }
            }
            
            while (index > 0) {
                recent_activity.erase(recent_activity.begin());
                index --;
            }
            return;
        }
    }
}

void AutoTrader::clear_event() {
    auto cur = steady_clock::now();
    long time = duration_cast<milliseconds>(cur - base_time).count();
    unsigned long index = 0;
    for (int i = 0 ; i < recent_activity.size(); i++) {
        if (time - recent_activity[i] > bound_time) {
            index += 1;
        } else {
            break;
        }
    }
    
    while (index > 0) {
        recent_activity.erase(recent_activity.begin());
        index --;
    }
}

unsigned long AutoTrader::weighted_average(const std::array<unsigned long, ReadyTraderGo::TOP_LEVEL_COUNT>& volume, const std::array<unsigned long, ReadyTraderGo::TOP_LEVEL_COUNT>& price) {
        // Weighted Average of prices and its corresponding volumes
        unsigned long sum = 0;
        unsigned long num = 0;
        int weight_count = 3;
        for (int i = 0; i < ReadyTraderGo::TOP_LEVEL_COUNT; i++) {
            sum += volume[i] * price[i];
            num += volume[i];
            if (num >= 300) break;
        }
        if (num == 0) return 0;
        return sum / num;
}

bool AutoTrader::trader_can_buy(unsigned long count, unsigned long price_to_buy) {
    /*Called every time we decide to buy ETF

    Count is the amount we want to buy
    The decision is that: 
    1. We can insert more order
    2. We have enough slot for message (including send, hedge, cancel reserved)
    2. We can hold more position
    */
    
    long to_buy = 0;
    
    for (auto order : Bids2Amount) {
        to_buy += order.second;
    }
    
    tend_to_own = to_buy;

    // Delete wash orders
    for (auto order : Asks2Price) {
        if (order.second == price_to_buy) {
            wait_event_space();
            // self.logger.info("insert for delete %d wash %d",  sells, len(self.recent_activity))
            SendCancelOrder(order.first);
            deleted.emplace(order.first);
            insert_event();
        }
    }
    
    return ongoing_order_num < 10 && (position + to_buy) <= (POSITION_LIMIT - (long)count) &&
        recent_activity.size() <= (message_limit - order_message_count) && count > 0 && count <= POSITION_LIMIT * 2;
}

bool AutoTrader::trader_can_sell(unsigned long count, unsigned long price_to_sell) {
    /* Called every time we decide to sell ETF

    Count is the amount we want to sell
    The decision is that: 
    1. We can insert more order
    2. We have enough slot for message (including send, hedge, cancel reserved)
    2. We can hold less position
    */
    
    long to_sell = 0;
    
    for (auto order : Asks2Amount) {
        to_sell += order.second;
    }
    
    tend_to_sell = to_sell;

    // Delete wash orders
    for (auto order : Bids2Price) {
        if (order.second == price_to_sell) {
            wait_event_space();
            // self.logger.info("insert for delete %d wash %d",  sells, len(self.recent_activity))
            SendCancelOrder(order.first);
            deleted.emplace(order.first);
            insert_event();
        }
    }
    
    return ongoing_order_num < 10 && (position - to_sell) >= -((long)POSITION_LIMIT - (long)count) &&
        recent_activity.size() <= (message_limit - order_message_count) && count > 0 && count <= POSITION_LIMIT * 2;
}

void AutoTrader::DisconnectHandler()
{
    BaseAutoTrader::DisconnectHandler();
    RLOG(LG_AT, LogLevel::LL_INFO) << "execution connection lost";
}

void AutoTrader::ErrorMessageHandler(unsigned long clientOrderId,
                                     const std::string& errorMessage)
{
    RLOG(LG_AT, LogLevel::LL_INFO) << "error with order " << clientOrderId << ": " << errorMessage;
    if (clientOrderId != 0 && ((Asks2Seq.count(clientOrderId) == 1) || (Bids2Seq.count(clientOrderId) == 1)))
    {
        //OrderStatusMessageHandler(clientOrderId, 0, 0, 0);
        wait_event_space();
        SendCancelOrder(clientOrderId);
        insert_event();
    }
}

void AutoTrader::HedgeFilledMessageHandler(unsigned long clientOrderId,
                                           unsigned long price,
                                           unsigned long volume)
{
    //RLOG(LG_AT, LogLevel::LL_INFO) << "hedge order " << clientOrderId << " filled for " << volume
    //                               << " lots at $" << price << " average price in cents";
}

void AutoTrader::cleanup(unsigned long sequence_number, int Order_Lifespan) {
    /*Called on the end of each order book update

    Delete all outdated orders that are Order_Lifespan sequence ago and the finished sequence
    In order not to retain order with outdated price, and also take on-going slots
    */

    // Delete the current sequence
    OB2Mid.erase(sequence_number);
    OB2BestBidPrice.erase(sequence_number);
    OB2BestBidVol.erase(sequence_number);
    OB2BestAskPrice.erase(sequence_number);
    OB2BestAskVol.erase(sequence_number);

    // Delete Old Orders
    for (auto order : Asks2Seq) {
        if (recent_activity.size() >= message_limit) {
            break;
        }
        
        if (sequence_number - order.second > Order_Lifespan && deleted.find(order.first) == deleted.end()) {
            // self.logger.info("insert for delete %d old %d", order, len(self.recent_activity))
            SendCancelOrder(order.first);
            insert_event();
        }
    }

    for (auto order : Bids2Seq) {
        if (recent_activity.size() >= message_limit) {
            break;
        }
        
        if (sequence_number - order.second > Order_Lifespan && deleted.find(order.first) == deleted.end()) {
            // self.logger.info("insert for delete %d old %d", order, len(self.recent_activity))
            SendCancelOrder(order.first);
            insert_event();
        }
    }
}

void AutoTrader::hedge_all() {
    if (unhedged == 0) return;
    
    if (unhedged > 0) {
        RLOG(LG_AT, LogLevel::LL_INFO) << "Hedging " << unhedged << " SELLLL";
        wait_event_space();                                           // Wait for another event space
        SendHedgeOrder(mNextMessageId++, Side::SELL, MIN_BID_NEARST_TICK, unhedged);
        insert_event();
    } else {
        RLOG(LG_AT, LogLevel::LL_INFO) << "Hedging " << unhedged << " BUYYYY";
        wait_event_space();                                          // Wait for another event space
        SendHedgeOrder(mNextMessageId++, Side::BUY, MAX_ASK_NEAREST_TICK, (unsigned long)-unhedged);
        insert_event();
    }
    
    unhedged = 0;
}

void AutoTrader::hedge_partial(bool trend) {
    if (unhedged == 0) return;
    
    long ratio = 10;
    
    //long to_be_hedged = ((hedge_fail - fail_limit + 1) * unhedged + ratio - 1) / ratio;
    long to_be_hedged = (unhedged + ratio - 1) / ratio;
    if (!trend) to_be_hedged = (unhedged + ratio - 1) / ratio;
    
    if (unhedged > 0) {
        if (to_be_hedged > unhedged) to_be_hedged = unhedged;
        RLOG(LG_AT, LogLevel::LL_INFO) << "Hedging " << to_be_hedged << " SELLLL";
        wait_event_space();                                           // Wait for another event space
        SendHedgeOrder(mNextMessageId++, Side::SELL, MIN_BID_NEARST_TICK, to_be_hedged);
        insert_event();
    } else {
        if (to_be_hedged < unhedged) to_be_hedged = unhedged;
        RLOG(LG_AT, LogLevel::LL_INFO) << "Hedging " << to_be_hedged << " BUYYYY";
        wait_event_space();                                          // Wait for another event space
        SendHedgeOrder(mNextMessageId++, Side::BUY, MAX_ASK_NEAREST_TICK, (unsigned long)-to_be_hedged);
        insert_event();
    }
    
    unhedged -= to_be_hedged;
}

// Compare Avg Version
/*void AutoTrader::handle_hedge(unsigned long future_price) {
    auto cur = steady_clock::now();
    long time = duration_cast<milliseconds>(cur - base_time).count();
    int fail_limit = 2;
    
    if (unhedged_start == -1) {
        last_future = future_price;
        return;
    }
    
    if (time - unhedged_start > 58000) {
      hedge_all();
      return;
    }
    
    if (last_future == 0) {
        last_future = (last_future * avg_count + future_price) / (avg_count + 1);
        avg_count ++;
    } else if (unhedged > 0) {
        // Sell Price too low
        if (future_sell_price <= position_price + 101 && future_sell_price > position_price) {
            hedge_all();
            hedge_fail = 0;
            return;
        }
    
        if (last_future > future_price) {
            if (hedge_fail + 1 < fail_limit) {
                hedge_fail ++;
                last_future = (last_future * avg_count + future_price) / (avg_count + 1);
                avg_count ++;
            } else {
                hedge_all();
                hedge_fail = 0;
            }
        } else {
            hedge_fail = 0;
            last_future = (last_future * avg_count + future_price) / (avg_count + 1);
            avg_count ++;
        }
    } else if (unhedged < 0){
    
        // Sell Price too low
        if (future_buy_price >= position_price - 101 && future_buy_price < position_price) {
            hedge_all();
            hedge_fail = 0;
            return;
        }
        
        if (last_future < future_price) {
            if (hedge_fail + 1 < fail_limit) {
                hedge_fail ++;
                last_future = (last_future * avg_count + future_price) / (avg_count + 1);
                avg_count ++;
            } else {
                hedge_all();
                hedge_fail = 0;
            }
        } else {
            hedge_fail = 0;
            last_future = (last_future * avg_count + future_price) / (avg_count + 1);
            avg_count ++;
        }
    }
}*/

// Local Avg Version
void AutoTrader::handle_hedge(unsigned long future_price) {
    auto cur = steady_clock::now();
    long time = duration_cast<milliseconds>(cur - base_time).count();
    unsigned long cur_avg = 0;
    
    if (recent_future.size() >= future_avg_size) recent_future.erase(recent_future.begin());
    recent_future.push_back(future_price);
    for (auto p : recent_future) {
        cur_avg += p;
    }
    cur_avg /= recent_future.size();
    
    if (unhedged_start == -1) {
        last_future = cur_avg;
        return;
    }
    
    if (time - unhedged_start > 58000) {
      hedge_all();
      return;
    }
    
    long dur = time - trend_start;
    if (dur < 2000) {
        fail_limit = 1;
    } else if (dur >= 2000 && dur < 5000) {
        fail_limit = 2;
    } else {
        fail_limit = 3;
    }
    if (unhedged > 0) {
        // Sell Price too low
        /*if (future_sell_price <= position_price + 101 && future_sell_price > position_price) {
            hedge_partial(false);
            hedge_fail = 0;
            return;
        }
        
        if (future_sell_price <= position_price) {
            last_future = cur_avg;
            return;
        }*/
    
        if (last_future > cur_avg) {
            RLOG(LG_AT, LogLevel::LL_INFO) << "BAD";
            if (hedge_fail + 1 < fail_limit) {
                hedge_fail ++;
            } else {
                hedge_fail ++;
                hedge_partial(true);
                //hedge_fail = 0;
            }
        } else {
            hedge_fail = 0;
        }
    } else if (unhedged < 0){
    
        // Sell Price too low
        /*if (future_buy_price >= position_price - 101 && future_buy_price < position_price) {
            hedge_partial(false);
            hedge_fail = 0;
            return;
        }
        
        if (future_buy_price >= position_price) {
            last_future = cur_avg;
            return;
        }*/
        
        if (last_future < cur_avg) {
            RLOG(LG_AT, LogLevel::LL_INFO) << "BAD";
            if (hedge_fail + 1 < fail_limit) {
                hedge_fail ++;
            } else {
                hedge_fail ++;
                hedge_partial(true);
                //hedge_fail = 0;
            }
        } else {
            hedge_fail = 0;
        }
    }
    
    last_future = cur_avg;
}

void AutoTrader::OrderBookMessageHandler(Instrument instrument,
                                         unsigned long sequenceNumber,
                                         const std::array<unsigned long, TOP_LEVEL_COUNT>& askPrices,
                                         const std::array<unsigned long, TOP_LEVEL_COUNT>& askVolumes,
                                         const std::array<unsigned long, TOP_LEVEL_COUNT>& bidPrices,
                                         const std::array<unsigned long, TOP_LEVEL_COUNT>& bidVolumes)
{
    /*RLOG(LG_AT, LogLevel::LL_INFO) << "order book received for " << instrument << " instrument"
                                   << ": ask prices: " << askPrices[0]
                                   << "; ask volumes: " << askVolumes[0]
                                   << "; bid prices: " << bidPrices[0]
                                   << "; bid volumes: " << bidVolumes[0];*/
    
    clear_event();
    //auto cur = steady_clock::now();
    //long long time = duration_cast<milliseconds>(cur - base_time).count();
    //fprintf(stderr, "time %lld\n", time);
    
    // Calculate the weighted average mid prices
    unsigned long bid_mid = weighted_average(bidVolumes, bidPrices);
    unsigned long ask_mid = weighted_average(askVolumes, askPrices);
    unsigned long mid_price = (bid_mid + ask_mid) / 2;
    
    // Store the data for the first order book came in at a time (either ETF or Futures)
    if (OB2Mid.find(sequenceNumber) == OB2Mid.end()) {
        OB2Mid[sequenceNumber] = mid_price;
        OB2BestAskPrice[sequenceNumber] = askPrices[0];
        OB2BestAskVol[sequenceNumber] = askVolumes[0];
        OB2BestBidPrice[sequenceNumber] = bidPrices[0];
        OB2BestBidVol[sequenceNumber] = bidVolumes[0];
        return;
    }

    // If the obtained data is outdated
    else if (sequenceNumber < last_seq) {
        RLOG(LG_AT, LogLevel::LL_INFO) << "Outdated data for number " << sequenceNumber << " already in " << last_seq;
        OB2Mid.erase(sequenceNumber);
        OB2BestAskPrice.erase(sequenceNumber);
        OB2BestAskVol.erase(sequenceNumber);
        OB2BestBidPrice.erase(sequenceNumber);
        OB2BestBidVol.erase(sequenceNumber);
        return;
    }

    // We get a updated lastest market data (i.e. The largest sequnece time fully obtained that we have not handled)
    else {
        // Initialization of trading variables
        last_seq = sequenceNumber;
        unsigned long etf_price = 0;
        unsigned long future_price = 0;
        unsigned long target_bid_price = 0;
        unsigned long target_ask_price = 0;
        bool should_buy = true;
        bool should_sell = true;
        long hedge_diff = 0;
        unsigned long order_round = 2;
        unsigned long order_amount = 10;
        unsigned long etf_diff = 0;
        unsigned long best_volumn = 0;
        unsigned long price_adjust = 600;
        unsigned long price_to_buy = 0;
        unsigned long price_to_sell = 0;
        unsigned long trade_bound = 100;
        
        // Get info about the two market and the best prices for ETF
        // Set desired Trade price and determine whether we win money by hedging
        if (instrument == Instrument::FUTURE) {
            future_price = mid_price;
            etf_price = OB2Mid.at(sequenceNumber);
            target_bid_price = OB2BestBidPrice.at(sequenceNumber);
            target_ask_price = OB2BestAskPrice.at(sequenceNumber);
            
            // Update hedge buy/sell price
            future_buy_price = askPrices[0];
            future_sell_price = bidPrices[0];

            // Calculate the price to buy/sell if we would like to
            etf_diff = target_ask_price - target_bid_price;
            price_to_buy = (etf_diff > price_adjust) ? (target_ask_price - price_adjust) : (target_ask_price - etf_diff + TICK_SIZE_IN_CENTS);
            price_to_sell = (etf_diff > price_adjust) ? (target_bid_price + price_adjust) : (target_bid_price + etf_diff - TICK_SIZE_IN_CENTS);

            // Determine whether hedge earns, and which side earns
            hedge_diff = (long)future_sell_price - (long)price_to_buy;
            should_buy = (price_to_buy) <= future_sell_price - trade_bound;
            should_sell = (price_to_sell) >= future_buy_price + trade_bound;
            if (price_to_sell > future_buy_price) {
                hedge_diff = (long)price_to_sell - (long)future_buy_price;
            }
            best_volumn = should_buy ? OB2BestAskVol.at(sequenceNumber) : OB2BestBidVol.at(sequenceNumber);
            
        } else {
            future_price = OB2Mid.at(sequenceNumber);
            etf_price = mid_price;
            target_bid_price = bidPrices[0];
            target_ask_price = askPrices[0];

            // Update hedge buy/sell price
            future_buy_price = OB2BestAskPrice.at(sequenceNumber);
            future_sell_price = OB2BestBidPrice.at(sequenceNumber);

            // Calculate the price to buy/sell if we would like to
            etf_diff = target_ask_price - target_bid_price;
            price_to_buy = (etf_diff > price_adjust) ? (target_ask_price - price_adjust) : (target_ask_price - etf_diff + TICK_SIZE_IN_CENTS);
            price_to_sell = (etf_diff > price_adjust) ? (target_bid_price + price_adjust) : (target_bid_price + etf_diff - TICK_SIZE_IN_CENTS);

            // Determine whether hedge earns, and which side earns
            hedge_diff = (long)future_sell_price - (long)price_to_buy;
            should_buy = (price_to_buy) <= future_sell_price - trade_bound;
            should_sell = (price_to_sell) >= future_buy_price + trade_bound;
            if (price_to_sell > future_buy_price) {
                hedge_diff = (long)price_to_sell - (long)future_buy_price;
            }
            best_volumn = should_buy ? askVolumes[0] : bidVolumes[0];
        }
        
        handle_hedge(future_price);
        
        // Order more if we can earn more
        // hedge_diff -= (trade_bound - 100);
        order_amount = hedge_diff <= 0 ? 0 : (hedge_diff / 100) * (hedge_diff / 100) * 2;
        order_amount = hedge_diff <= 0 ? 0 : LOT_SIZE * hedge_diff / 50;
        
        // Determine whether becoming market taker would earn
        bool exceed_fee = (etf_diff == 100) && (best_volumn > (3 * order_round));

        // TRADE LOOP
        //for (int i = 0; i < order_round; i++) {
        if (should_buy && ((POSITION_LIMIT - position - tend_to_own) < order_amount)) {
            order_amount = POSITION_LIMIT - position - tend_to_own;
        }
        
        if (should_sell && ((POSITION_LIMIT + position - tend_to_sell) < order_amount)) {
            order_amount = POSITION_LIMIT + position - tend_to_sell;
        }

        // Buyside: If we can buy, with valid order book, should buy by hedge and would win
        if (should_buy && trader_can_buy(order_amount, price_to_buy) && target_bid_price != 0 && target_ask_price != 0 && (etf_diff > 100 || exceed_fee)) {
            //if (unhedged < 0) hedge_all();
            
            unsigned long cur_bid_id = mNextMessageId++;
            
            // Send buy order
            SendInsertOrder(cur_bid_id, Side::BUY, price_to_buy, order_amount, (etf_diff > 100) ? Lifespan::GOOD_FOR_DAY : Lifespan::FILL_AND_KILL);
            insert_event();

            // Log the action
            tend_to_own += order_amount;
            Bids2Seq[cur_bid_id] = sequenceNumber;
            Bids2Amount[cur_bid_id] = order_amount;
            Bids2Price[cur_bid_id] = price_to_buy;
            
            RLOG(LG_AT, LogLevel::LL_INFO) << "Hedge Buying Order" << cur_bid_id << " for price " << price_to_buy << " vol " << order_amount << " diff is " << hedge_diff
                            << " event " << recent_activity.size() << " position " << position;

            ongoing_order_num += 1;

            if (etf_diff == 100) {
                RLOG(LG_AT, LogLevel::LL_INFO) << "Paying the fee";
            }
        }

        // Sellside: If we can sell, with valid order book, should sell by hedge and would win
        //RLOG(LG_AT, LogLevel::LL_INFO) << "Sell side " << trader_can_sell(order_amount, price_to_sell) << " " << (etf_diff > 100 || exceed_fee);
        if (should_sell && trader_can_sell(order_amount, price_to_sell) && target_bid_price != 0 && target_ask_price != 0 && (etf_diff > 100 || exceed_fee)) {
            //if (unhedged > 0) hedge_all();
            
            unsigned long cur_ask_id = mNextMessageId++;

            // Send sell order
            SendInsertOrder(cur_ask_id, Side::SELL, price_to_sell, order_amount, (etf_diff > 100) ? Lifespan::GOOD_FOR_DAY : Lifespan::FILL_AND_KILL);
            insert_event();

            // Log the action
            tend_to_sell += order_amount;
            Asks2Seq[cur_ask_id] = sequenceNumber;
            Asks2Amount[cur_ask_id] = order_amount;
            Asks2Price[cur_ask_id] = price_to_sell;
            
            RLOG(LG_AT, LogLevel::LL_INFO) << "Hedge Selling Order" << cur_ask_id << " for price " << price_to_sell << " vol " << order_amount << " diff is " << hedge_diff
                            << " event " << recent_activity.size() << " position " << position;
                
            ongoing_order_num += 1;

            if (etf_diff == 100) {
                RLOG(LG_AT, LogLevel::LL_INFO) << "Paying the fee";
            }
        }
            
        //}
        
        // Clean up current sequence and old orders
        cleanup(sequenceNumber, 5);
        return;
    
    }
}

void AutoTrader::OrderFilledMessageHandler(unsigned long clientOrderId,
                                           unsigned long price,
                                           unsigned long volume)
{
    RLOG(LG_AT, LogLevel::LL_INFO) << "order " << clientOrderId << " filled for " << volume
                                   << " lots at $" << price << " cents " << recent_activity.size() << " events";
    
    long prev_unhedged = unhedged;
    
    // Bid Order Fill
    if (Bids2Amount.find(clientOrderId) != Bids2Amount.end()) {
        if (position >= 0 || (position + volume < 0)) {
            position_price = (position_price * position + volume * price) / (position + volume);
        } else {
            position_price = price;
        }
        position += (long)volume;                                     // Current position update
        tend_to_own -= volume;                                        // To be bought amount update
        unsigned long remain = Bids2Amount[clientOrderId] - volume; // Specific Order Remain Update
        Bids2Amount.erase(clientOrderId);
        Bids2Amount[clientOrderId] = remain;
        
        /*if (unhedged == 0) {
            unhedged += (long)volume;
        } else if (unhedged > 0) {
            unhedged += (long)volume;
        } else if (unhedged + (long)volume < 0) {
            unhedged += (long)volume;
        } else {
            unhedged += (long)volume;
            hedge_all();
        }*/
        
        unhedged += (long) volume;
        if (prev_unhedged <= 0 && unhedged >= 0) {
            auto cur = steady_clock::now();
            long time = duration_cast<milliseconds>(cur - base_time).count();
            trend_start = time;
            hedge_all();
        }
        
        if (prev_unhedged <= 10 && prev_unhedged >= -10 && (unhedged > 10 || unhedged < -10)) {
            auto cur = steady_clock::now();
            long time = duration_cast<milliseconds>(cur - base_time).count();
            unhedged_start = time;
        }
        
        /*wait_event_space();                                           // Wait for another event space
        SendHedgeOrder(mNextMessageId++, Side::SELL, MIN_BID_NEARST_TICK, volume);
        insert_event();*/
    }

    // Ask Order Fill
    else if (Asks2Amount.find(clientOrderId) != Asks2Amount.end()) {
        if (position <= 0 || (position - volume > 0)) {
            position_price = (position_price * position - volume * price) / (position - volume);
        } else {
            position_price = price;
        }
        
        position -= (long)volume;                                    // Current position update
        tend_to_sell -= volume;                                      // To be sold amount update
        unsigned long remain = Asks2Amount[clientOrderId] - volume;
        Asks2Amount.erase(clientOrderId);
        Asks2Amount[clientOrderId] = remain;
        
        /*if (unhedged == 0) {
            unhedged -= (long)volume;
        } else if (unhedged < 0 || (unhedged - (long)volume > 0)) {
            unhedged -= (long)volume;
        } else {
            unhedged -= (long)volume;
            hedge_all();
        }*/
        
        unhedged -= (long)volume;
        
        if (prev_unhedged >= 0 && unhedged <= 0) {
            auto cur = steady_clock::now();
            long time = duration_cast<milliseconds>(cur - base_time).count();
            trend_start = time;
            hedge_all();
        }
        
        if (prev_unhedged <= 10 && prev_unhedged >= -10 && (unhedged > 10 || unhedged < -10)) {
            auto cur = steady_clock::now();
            long time = duration_cast<milliseconds>(cur - base_time).count();
            unhedged_start = time;
        }
        
        /*wait_event_space();                                          // Wait for another event space
        SendHedgeOrder(mNextMessageId++, Side::BUY, MAX_ASK_NEAREST_TICK, volume);
        insert_event();*/
    }
    
    RLOG(LG_AT, LogLevel::LL_INFO) << "Current Position is " << position << " to buy " << tend_to_own << " to sell " << tend_to_sell;
}

void AutoTrader::OrderStatusMessageHandler(unsigned long clientOrderId,
                                           unsigned long fillVolume,
                                           unsigned long remainingVolume,
                                           signed long fees)
{
    // Delete fully filled orders
    if (remainingVolume == 0) {
        ongoing_order_num -= 1;

        if (deleted.find(clientOrderId) != deleted.end()) {
            deleted.erase(clientOrderId);
        }
        
        if (Bids2Price.find(clientOrderId) != Bids2Price.end()) {
            Bids2Price.erase(clientOrderId);
            Bids2Amount.erase(clientOrderId);
            Bids2Seq.erase(clientOrderId);
        } else {
            Asks2Price.erase(clientOrderId);
            Asks2Amount.erase(clientOrderId);
            Asks2Seq.erase(clientOrderId);
        }
    }
}

void AutoTrader::TradeTicksMessageHandler(Instrument instrument,
                                          unsigned long sequenceNumber,
                                          const std::array<unsigned long, TOP_LEVEL_COUNT>& askPrices,
                                          const std::array<unsigned long, TOP_LEVEL_COUNT>& askVolumes,
                                          const std::array<unsigned long, TOP_LEVEL_COUNT>& bidPrices,
                                          const std::array<unsigned long, TOP_LEVEL_COUNT>& bidVolumes)
{
    /*RLOG(LG_AT, LogLevel::LL_INFO) << "trade ticks received for " << instrument << " instrument"
                                   << ": ask prices: " << askPrices[0]
                                   << "; ask volumes: " << askVolumes[0]
                                   << "; bid prices: " << bidPrices[0]
                                   << "; bid volumes: " << bidVolumes[0];*/
}
