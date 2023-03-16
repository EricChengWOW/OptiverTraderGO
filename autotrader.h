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
#ifndef CPPREADY_TRADER_GO_AUTOTRADER_H
#define CPPREADY_TRADER_GO_AUTOTRADER_H

#include <array>
#include <memory>
#include <string>
#include <unordered_set>
#include <tuple>
#include <unordered_map>
#include <vector>
#include <chrono>
#include <map>

#include <boost/asio/io_context.hpp>

#include <ready_trader_go/baseautotrader.h>
#include <ready_trader_go/types.h>

using namespace std::chrono;

class AutoTrader : public ReadyTraderGo::BaseAutoTrader
{
public:
    explicit AutoTrader(boost::asio::io_context& context);
    
    void insert_event();
    void wait_event_space();
    void clear_event();
    unsigned long weighted_average(const std::array<unsigned long, ReadyTraderGo::TOP_LEVEL_COUNT>& volume, const std::array<unsigned long, ReadyTraderGo::TOP_LEVEL_COUNT>& price);
    bool trader_can_buy(unsigned long count, unsigned long price_to_buy);
    bool trader_can_sell(unsigned long count, unsigned long price_to_sell);
    void cleanup(unsigned long sequence_number, int Order_Lifespan);
    void hedge_all();
    void handle_hedge(unsigned long future_price);
    void hedge_partial(bool trend);
    
    // Called when the execution connection is lost.
    void DisconnectHandler() override;

    // Called when the matching engine detects an error.
    // If the error pertains to a particular order, then the client_order_id
    // will identify that order, otherwise the client_order_id will be zero.
    void ErrorMessageHandler(unsigned long clientOrderId,
                             const std::string& errorMessage) override;

    // Called when one of your hedge orders is filled, partially or fully.
    //
    // The price is the average price at which the order was (partially) filled,
    // which may be better than the order's limit price. The volume is
    // the number of lots filled at that price.
    //
    // If the order was unsuccessful, both the price and volume will be zero.
    void HedgeFilledMessageHandler(unsigned long clientOrderId,
                                   unsigned long price,
                                   unsigned long volume) override;

    // Called periodically to report the status of an order book.
    // The sequence number can be used to detect missed or out-of-order
    // messages. The five best available ask (i.e. sell) and bid (i.e. buy)
    // prices are reported along with the volume available at each of those
    // price levels.
    void OrderBookMessageHandler(ReadyTraderGo::Instrument instrument,
                                 unsigned long sequenceNumber,
                                 const std::array<unsigned long, ReadyTraderGo::TOP_LEVEL_COUNT>& askPrices,
                                 const std::array<unsigned long, ReadyTraderGo::TOP_LEVEL_COUNT>& askVolumes,
                                 const std::array<unsigned long, ReadyTraderGo::TOP_LEVEL_COUNT>& bidPrices,
                                 const std::array<unsigned long, ReadyTraderGo::TOP_LEVEL_COUNT>& bidVolumes) override;

    // Called when one of your orders is filled, partially or fully.
    void OrderFilledMessageHandler(unsigned long clientOrderId,
                                   unsigned long price,
                                   unsigned long volume) override;

    // Called when the status of one of your orders changes.
    // The fill volume is the number of lots already traded, remaining volume
    // is the number of lots yet to be traded and fees is the total fees paid
    // or received for this order.
    // Remaining volume will be set to zero if the order is cancelled.
    void OrderStatusMessageHandler(unsigned long clientOrderId,
                                   unsigned long fillVolume,
                                   unsigned long remainingVolume,
                                   signed long fees) override;

    // Called periodically when there is trading activity on the market.
    // The five best ask (i.e. sell) and bid (i.e. buy) prices at which there
    // has been trading activity are reported along with the aggregated volume
    // traded at each of those price levels.
    // If there are less than five prices on a side, then zeros will appear at
    // the end of both the prices and volumes arrays.
    void TradeTicksMessageHandler(ReadyTraderGo::Instrument instrument,
                                  unsigned long sequenceNumber,
                                  const std::array<unsigned long, ReadyTraderGo::TOP_LEVEL_COUNT>& askPrices,
                                  const std::array<unsigned long, ReadyTraderGo::TOP_LEVEL_COUNT>& askVolumes,
                                  const std::array<unsigned long, ReadyTraderGo::TOP_LEVEL_COUNT>& bidPrices,
                                  const std::array<unsigned long, ReadyTraderGo::TOP_LEVEL_COUNT>& bidVolumes) override;

private:

    unsigned long mNextMessageId = 1;
    signed long position = 0;
    
    // Store the ongoing orders
    std::unordered_map<unsigned long, unsigned long> Asks2Seq;
    std::unordered_map<unsigned long, unsigned long> Asks2Amount;
    std::unordered_map<unsigned long, unsigned long> Asks2Price;
    std::unordered_map<unsigned long, unsigned long> Bids2Seq;
    std::unordered_map<unsigned long, unsigned long> Bids2Amount;
    std::unordered_map<unsigned long, unsigned long> Bids2Price;
    
    // Store the deleted orders for wash order
    std::unordered_set<unsigned long> deleted;
    
    // Store the order books for matching Future and ETF
    std::unordered_map<unsigned long, unsigned long> OB2Mid;
    std::unordered_map<unsigned long, unsigned long> OB2BestBidPrice;
    std::unordered_map<unsigned long, unsigned long> OB2BestBidVol;
    std::unordered_map<unsigned long, unsigned long> OB2BestAskPrice;
    std::unordered_map<unsigned long, unsigned long> OB2BestAskVol;
                                             
    // Store the recent activity time
    std::vector<long long> recent_activity;
    
    // Store the recent future mid price
    std::vector<unsigned long> recent_future;
    
    unsigned long last_seq = 0;
    unsigned long ongoing_order_num = 0;
    unsigned long tend_to_own = 0;
    unsigned long tend_to_sell = 0;
    unsigned long history_limit = 4;
    unsigned long position_price = 0;
    int future_sell_price = 0;
    int future_buy_price = 0;
    
    unsigned long future_avg_size = 3;
    long long bound_time = 1050;
    unsigned long message_limit = 48;
    unsigned long order_message_count = 3;
    unsigned long last_future = 0;
    unsigned long avg_count = 0;
    int fail_limit = 2;
    long unhedged = 0;
    long unhedged_start = -1;
    long trend_start = 0;
    int hedge_fail = 0;
    steady_clock::time_point base_time = steady_clock::now();
    steady_clock::time_point last_trade = steady_clock::now();
};

#endif //CPPREADY_TRADER_GO_AUTOTRADER_H
